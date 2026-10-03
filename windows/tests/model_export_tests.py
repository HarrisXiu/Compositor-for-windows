import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from export_models import compare_tensor, compare_mask, deform_conv_export, run


class ModelExportTests(unittest.TestCase):
    def test_identical_tensors_pass(self):
        result = compare_tensor(np.ones((2, 3), np.float32), np.ones((2, 3), np.float32))
        self.assertTrue(result["passed"])
        self.assertEqual(result["max_abs_error"], 0)

    def test_shape_and_nonfinite_values_fail(self):
        for actual in (np.ones((3, 2)), np.full((2, 3), np.nan), np.full((2, 3), np.inf)):
            with self.subTest(actual=actual):
                self.assertFalse(compare_tensor(np.ones((2, 3)), actual)["passed"])
        self.assertFalse(compare_tensor(np.array([]), np.array([]))["passed"])

    def test_runtime_feed_requires_exact_input_count(self):
        from types import SimpleNamespace

        session = SimpleNamespace(get_inputs=lambda: [SimpleNamespace(name="image")])
        with self.assertRaises(ValueError):
            run(session, ())

    def test_large_error_fails(self):
        self.assertFalse(compare_tensor(np.zeros((2, 3)), np.ones((2, 3)))["passed"])

    def test_mask_iou_and_probability_error(self):
        mask = np.array([[0.1, 0.9], [0.2, 0.8]], np.float32)
        self.assertTrue(compare_mask(mask, mask)["passed"])
        self.assertFalse(compare_mask(mask, 1 - mask)["passed"])
        self.assertFalse(compare_mask(mask, mask + 0.01)["passed"])
        self.assertTrue(compare_mask(np.zeros((2, 2)), np.zeros((2, 2)))["passed"])
        self.assertFalse(compare_mask(mask, np.full((2, 2), np.nan))["passed"])

    def test_mask_candidates_are_checked_individually(self):
        expected = np.ones((1, 2, 64, 64), np.float32)
        expected[:, 1] = 0
        expected[:, 1, 0, 0] = 1
        actual = expected.copy()
        actual[:, 1] = 0
        self.assertFalse(compare_mask(expected, actual)["passed"])

    def test_deform_convolution_accepts_integer_parameters(self):
        import torch
        from torchvision.ops import deform_conv2d

        image = torch.randn(2, 2, 5, 7)
        weight = torch.randn(3, 2, 3, 3)
        offset = torch.randn(2, 18, 5, 7)
        args = (image, offset, weight)
        torch.testing.assert_close(deform_conv_export(*args, padding=1), deform_conv2d(*args, padding=1), atol=3e-5, rtol=3e-5)

    def test_deform_convolution_lowering_matches_torchvision(self):
        import torch
        from torchvision.ops import deform_conv2d

        torch.manual_seed(41)
        for groups, offset_groups, stride, dilation in ((1, 1, 1, 1), (2, 1, 1, 1), (1, 2, 2, 1), (2, 2, 1, 2)):
            with self.subTest(groups=groups, offset_groups=offset_groups, stride=stride, dilation=dilation):
                image = torch.randn(1, 4, 9, 11)
                weight = torch.randn(6, 4 // groups, 3, 3)
                bias = torch.randn(6)
                h = (9 + 2 - dilation * 2 - 1) // stride + 1
                w = (11 + 2 - dilation * 2 - 1) // stride + 1
                offsets = torch.randn(1, offset_groups * 18, h, w) * 1.5
                masks = torch.rand(1, offset_groups * 9, h, w)
                args = (image, offsets, weight, bias, (stride, stride), (1, 1), (dilation, dilation), masks)
                torch.testing.assert_close(deform_conv_export(*args), deform_conv2d(*args), atol=3e-5, rtol=3e-5)

    def test_lowering_exports_to_standard_onnx(self):
        import onnx
        import onnxruntime as ort
        import torch

        class DeformLayer(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.weight = torch.nn.Parameter(torch.randn(2, 2, 3, 3))
                self.bias = torch.nn.Parameter(torch.randn(2))

            def forward(self, image, offset, mask):
                return deform_conv_export(image, offset, self.weight, self.bias, padding=(1, 1), mask=mask)

        torch.manual_seed(19)
        model = DeformLayer().eval()
        inputs = (torch.randn(1, 2, 7, 9), torch.randn(1, 18, 7, 9), torch.rand(1, 9, 7, 9))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "deform.onnx"
            torch.onnx.export(model, inputs, str(path), input_names=["image", "offset", "mask"], output_names=["output"], opset_version=17, dynamo=False)
            onnx.checker.check_model(str(path), full_check=True)
            session = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
            actual = session.run(None, dict(zip(("image", "offset", "mask"), (x.numpy() for x in inputs))))[0]
            with torch.inference_mode():
                expected = model(*inputs).numpy()
            np.testing.assert_allclose(actual, expected, atol=3e-5, rtol=3e-5)
            self.assertTrue(all(node.domain in ("", "ai.onnx") for node in onnx.load(str(path)).graph.node))


if __name__ == "__main__":
    unittest.main()
