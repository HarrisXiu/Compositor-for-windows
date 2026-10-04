// SPDX-License-Identifier: MIT
#include "ai_selection.h"
#include "ai_models.h"
#include "editor.h"
#include "language.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <limits>
#include <thread>
using namespace compositor;
namespace {
class Fixture final : public AiSelectionService {
  public:
    std::atomic<int> calls{0}, delay{30};
    bool fail = false;
    AiSelectionResult subject(const QImage &image, std::shared_ptr<AiCancellation> cancel) override {
        wait(cancel);
        QImage mask(image.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 10; y < 30; ++y)
            for (int x = 10; x < 30; ++x)
                mask.scanLine(y)[x] = 255;
        return {mask, "CPU"};
    }
    AiSelectionResult object(const QImage &image, const QString &, const QVector<QPointF> &points,
                             const QVector<int> &labels, std::shared_ptr<AiCancellation> cancel) override {
        wait(cancel);
        QImage mask(image.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        for (qsizetype i = 0; i < points.size(); ++i)
            for (int y = std::max(0, int(points[i].y())-3); y < std::min(image.height(), int(points[i].y())+4); ++y)
                for (int x = std::max(0, int(points[i].x())-3); x < std::min(image.width(), int(points[i].x())+4); ++x)
                    mask.scanLine(y)[x] = labels[i] ? 255 : 0;
        return {mask, "CPU"};
    }
    void wait(const std::shared_ptr<AiCancellation> &cancel) {
        ++calls;
        for (int i = 0; i < delay; ++i) { cancel->check(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        cancel->check();
        if (fail) throw std::runtime_error("Fixture model failure");
    }
};
QAction *action(EditorWindow &w, const QString &name) {
    for (auto a : w.findChildren<QAction *>())
        if (a->property("layerAction").toString() == name) return a;
    return nullptr;
}
EditorPage *page(EditorWindow &w) { return qobject_cast<EditorPage *>(w.findChild<QTabWidget *>()->currentWidget()); }
std::shared_ptr<Fixture> prepare(EditorWindow &w) {
    auto p = page(w);
    p->document = Document::create({64,64});
    QImage image(64,64,QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::red);
    p->document.addImage("Layer",image);
    p->canvas->refresh();
    p->canvas->selectAll();
    auto fixture = std::make_shared<Fixture>();
    p->aiSelection = fixture;
    return fixture;
}
QPushButton *ok(QDialog *dialog) { return dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok); }
AiTensor reference(const QDir &dir, const QString &name, const QJsonObject &meta) {
    QFile file(dir.filePath(meta.value("file").toString()));
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing AI reference tensor");
    QVector<qint64> shape;
    for (auto dim : meta.value("shape").toArray()) shape.append(qint64(dim.toDouble()));
    AiTensor tensor{name,shape,AiTensorType::Float32,qUncompress(file.readAll())};
    tensor.validate();
    return tensor;
}
double iou(const QImage &a, const QImage &b) {
    quint64 intersection=0, total=0;
    for (int y=0;y<a.height();++y)
        for (int x=0;x<a.width();++x) {
            const bool av=a.constScanLine(y)[x]>0,bv=b.constScanLine(y)[x]>0;
            intersection+=av&&bv; total+=av||bv;
        }
    return total ? double(intersection)/total : 1;
}
}
class SelectionTests : public QObject {
    Q_OBJECT
  private slots:
    void init() { UiLanguage::instance().setLanguage("en",false); }
    void subjectMaskGeometryAndTransparency() {
        QImage image(4,2,QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white); image.setPixelColor(3,1,Qt::transparent);
        auto logits=AiTensor::floats("logits",{1,1,2,2},{-2,2,-2,2});
        auto mask=aiSubjectSelection(logits,image);
        QCOMPARE(mask.size(),image.size());
        QCOMPARE(mask.constScanLine(0)[1],uchar(0));
        QCOMPARE(mask.constScanLine(0)[2],uchar(255));
        QCOMPARE(mask.constScanLine(1)[3],uchar(0));
        auto canceled=std::make_shared<AiCancellation>();canceled->cancel();
        QVERIFY_EXCEPTION_THROWN(aiSubjectSelection(logits,image,canceled),AiCancelled);
        logits=AiTensor::floats("logits",{1,1,1,1},{std::numeric_limits<float>::infinity()});
        QVERIFY_EXCEPTION_THROWN(aiSubjectSelection(logits,image),std::exception);
    }
    void mobileCandidateSelectionAndValidation() {
        QImage image(2,1,QImage::Format_RGBA8888_Premultiplied);image.fill(Qt::white);
        AiRunResult run;
        run.outputs={AiTensor::floats("masks",{1,4,1,2},{2,2,2,-2,-2,-2,-2,2}),
            AiTensor::floats("iou_predictions",{1,4},{99,.2f,.1f,.8f}),
            AiTensor::floats("low_res_masks",{1,4,256,256},QVector<float>(4*256*256))};
        auto result=aiObjectSelection(run,image,AiImageKind::MobileSAM);
        QCOMPARE(result.candidate,3);
        QCOMPARE(result.mask.constScanLine(0)[0],uchar(0));
        QCOMPARE(result.mask.constScanLine(0)[1],uchar(255));
        run.outputs[1]=AiTensor::floats("iou_predictions",{1,4},{99,.2f,.1f,std::numeric_limits<float>::quiet_NaN()});
        QVERIFY_EXCEPTION_THROWN(aiObjectSelection(run,image,AiImageKind::MobileSAM),std::exception);
    }
    void selectionCombination() {
        QImage old(2,1,QImage::Format_Grayscale8),mask(2,1,QImage::Format_Grayscale8);
        old.fill(0);old.scanLine(0)[0]=255;mask.fill(255);
        QCOMPARE(combineAiSelection(old,mask,0),mask);
        QCOMPARE(combineAiSelection(old,mask,1),mask);
        QCOMPARE(combineAiSelection(old,mask,2).constScanLine(0)[0],uchar(0));
        QCOMPARE(combineAiSelection(old,mask,3),old);
    }
    void missingModelsFailClosed() {
        QTemporaryDir dir;auto service=createAiSelectionService(dir.path());
        QImage image(32,32,QImage::Format_RGB32);image.fill(Qt::white);
        QVERIFY_EXCEPTION_THROWN(service->subject(image,std::make_shared<AiCancellation>()),std::exception);
        QVERIFY_EXCEPTION_THROWN(service->object(image,"sam2",{{5,5}},{1},std::make_shared<AiCancellation>()),std::exception);
        QVERIFY_EXCEPTION_THROWN(service->object(image,"sam2",{{-1,5}},{1},std::make_shared<AiCancellation>()),std::exception);
    }
    void subjectUndoAndCancel_data() { QTest::addColumn<bool>("cancel");QTest::newRow("commit")<<false;QTest::newRow("cancel")<<true; }
    void subjectUndoAndCancel() {
        QFETCH(bool,cancel);EditorWindow w;auto fixture=prepare(w);auto p=page(w);
        auto original=p->session.selection;const int count=p->history.count();
        if(cancel) {
            fixture->delay=200;
            QTimer::singleShot(0,&w,[&] { auto progress=w.findChild<QProgressDialog *>("aiSubjectProgress");QVERIFY(progress);progress->cancel();progress->reject(); });
        }
        action(w,"Select Subject")->trigger();
        QVERIFY(!p->isModified());
        QCOMPARE(p->history.count(),count+(cancel?0:1));
        if(cancel) { QTest::qWait(50);QCOMPARE(p->session.selection,original); }
        else { QCOMPARE(p->session.selection.constScanLine(20)[20],uchar(255));p->history.undo();QCOMPARE(p->session.selection,original); }
    }
    void objectPreviewCommitCancel_data() { QTest::addColumn<bool>("cancel");QTest::newRow("commit")<<false;QTest::newRow("cancel")<<true; }
    void objectPreviewCommitCancel() {
        QFETCH(bool,cancel);EditorWindow w;prepare(w);auto p=page(w);
        auto original=p->session.selection;const int count=p->history.count();
        action(w,"Select Object…")->trigger();auto dialog=w.findChild<QDialog *>("aiObjectDialog");QVERIFY(dialog);
        emit p->canvas->aiPointPicked({20,20},false);
        QTRY_VERIFY(ok(dialog)->isEnabled());
        QCOMPARE(p->session.selection.constScanLine(20)[20],uchar(255));
        QCOMPARE(p->history.count(),count);
        if(cancel) dialog->reject();else ok(dialog)->click();
        QCOMPARE(p->history.count(),count+(cancel?0:1));QVERIFY(!p->isModified());
        if(cancel) QCOMPARE(p->session.selection,original);
        else { p->history.undo();QCOMPARE(p->session.selection,original);p->history.redo();QCOMPARE(p->session.selection.constScanLine(10)[10],uchar(0)); }
    }
    void latestPromptAndClearPending() {
        EditorWindow w;auto fixture=prepare(w);fixture->delay=100;auto p=page(w);
        const auto original=p->session.selection;const auto count=p->history.count();
        action(w,"Select Object…")->trigger();auto dialog=w.findChild<QDialog *>("aiObjectDialog");
        emit p->canvas->aiPointPicked({20,20},false);QTRY_VERIFY(fixture->calls>0);
        emit p->canvas->aiPointPicked({20,20},true);
        emit p->canvas->aiPointPicked({40,40},false);
        QTRY_VERIFY(ok(dialog)->isEnabled());
        QCOMPARE(p->session.selection.constScanLine(20)[20],uchar(0));
        QCOMPARE(p->session.selection.constScanLine(40)[40],uchar(255));
        emit p->canvas->aiPointPicked({10,10},false);
        dialog->findChild<QPushButton *>("aiClearPoints")->click();
        QTest::qWait(200);QCOMPARE(p->session.selection,original);QVERIFY(!ok(dialog)->isEnabled());
        dialog->reject();QCOMPARE(p->history.count(),count);
    }
    void externalEditAndTabChangeInvalidation() {
        EditorWindow w;auto fixture=prepare(w);fixture->delay=100;auto p=page(w);const auto original=p->session.selection;
        action(w,"Select Object…")->trigger();emit p->canvas->aiPointPicked({20,20},false);
        action(w,"Deselect")->trigger();QTest::qWait(200);QVERIFY(p->session.selection.isNull());
        p->canvas->replaceSelection(original,"Restore");
        action(w,"Select Object…")->trigger();emit p->canvas->aiPointPicked({20,20},false);
        auto tabs=w.findChild<QTabWidget *>();auto other=new EditorPage(Document::create({64,64}));
        tabs->addTab(other,"Other");tabs->setCurrentWidget(other);QTest::qWait(200);
        QCOMPARE(p->session.selection,original);QVERIFY(page(w)!=p);
    }
    void canvasPointPickingAndToolChange() {
        EditorWindow w;prepare(w);auto p=page(w);w.resize(960,640);w.show();QTest::qWait(20);p->canvas->zoomTo(1);
        const auto image=p->document.active()->image;
        action(w,"Select Object…")->trigger();
        const int inset=p->session.view.rulers?24:0;
        const QPoint position((p->canvas->width()+inset-64)/2+20,(p->canvas->height()+inset-64)/2+20);
        QTest::mouseClick(p->canvas,Qt::LeftButton,{},position);
        QPointer<QDialog> dialog=w.findChild<QDialog *>("aiObjectDialog");QTRY_VERIFY(ok(dialog)->isEnabled());
        QCOMPARE(p->document.active()->image,image);
        QTest::keyClick(p->canvas,Qt::Key_Backspace);QVERIFY(!ok(dialog)->isEnabled());
        QTest::keyClick(p->canvas,Qt::Key_Escape);QVERIFY(!dialog || !dialog->isVisible());
    }
    void panelEditsDoNotCapturePreview() {
        EditorWindow w;prepare(w);auto p=page(w);auto original=p->session.selection;
        action(w,"Select Object…")->trigger();emit p->canvas->aiPointPicked({20,20},false);
        auto dialog=w.findChild<QDialog *>("aiObjectDialog");QTRY_VERIFY(ok(dialog)->isEnabled());
        p->edit("Panel edit",[](Document &d) { d.active()->metadata["opacity"]=.5; });
        QCOMPARE(p->session.selection,original);p->history.undo();QCOMPARE(p->session.selection,original);
        action(w,"Select Object…")->trigger();emit p->canvas->aiPointPicked({20,20},false);
        p->canvas->clearSelection();QTest::qWait(100);QVERIFY(p->session.selection.isNull());
    }
    void objectFailureLeavesOriginalSelection() {
        EditorWindow w;auto fixture=prepare(w);fixture->fail=true;auto p=page(w);auto original=p->session.selection;
        action(w,"Select Object…")->trigger();auto dialog=w.findChild<QDialog *>("aiObjectDialog");
        emit p->canvas->aiPointPicked({20,20},true);QVERIFY(!ok(dialog)->isEnabled());
        emit p->canvas->aiPointPicked({20,20},false);QTRY_VERIFY(fixture->calls>0);QTest::qWait(100);
        QCOMPARE(p->session.selection,original);QVERIFY(!ok(dialog)->isEnabled());
        fixture->fail=false;emit p->canvas->aiPointPicked({40,40},false);QTRY_VERIFY(ok(dialog)->isEnabled());
        fixture->fail=true;const int previous=fixture->calls;
        dialog->findChild<QComboBox *>("aiObjectModel")->setCurrentIndex(1);
        QTRY_VERIFY(fixture->calls>previous);QTest::qWait(100);
        QCOMPARE(p->session.selection,original);QVERIFY(!ok(dialog)->isEnabled());dialog->reject();
    }
    void objectDialogLanguages_data() {
        QTest::addColumn<QString>("language");
        QTest::newRow("English")<<QString("en");QTest::newRow("Chinese")<<QString("zh_CN");QTest::newRow("Japanese")<<QString("ja_JP");
    }
    void objectDialogLanguages() {
        QFETCH(QString,language);UiLanguage::instance().setLanguage(language,false);
        EditorWindow w;prepare(w);auto p=page(w);w.resize(1000,680);w.show();QTest::qWait(20);p->canvas->zoomTo(4);
        action(w,"Select Object…")->trigger();auto dialog=w.findChild<QDialog *>("aiObjectDialog");
        emit p->canvas->aiPointPicked({20,20},false);QTRY_VERIFY(ok(dialog)->isEnabled());
        QCOMPARE(dialog->windowTitle(),uiText("Select Object"));
        if(qEnvironmentVariableIsSet("COMPOSITOR_AI_SELECTION_SCREENSHOTS")) {
            dialog->grab().save("artifacts/ai2-object-dialog-"+language+".png");
            w.grab().save("artifacts/ai2-canvas-"+language+".png");
        }
        dialog->reject();
    }
    void realEditorSelection_data() {
        QTest::addColumn<QString>("id");
        for(auto id:{"birefnet-lite","sam2","mobilesam"})QTest::newRow(id)<<QString(id);
    }
    void realEditorSelection() {
        if(!qEnvironmentVariableIsSet("COMPOSITOR_AI_SELECTION_MODEL_ROOT"))QSKIP("Opt-in real editor AI selection");
        QFETCH(QString,id);
        try {
            QDir directory(qEnvironmentVariable("COMPOSITOR_AI_SELECTION_REFERENCES"));
            QFile manifest(directory.filePath("references.json"));QVERIFY(manifest.open(QIODevice::ReadOnly));
            auto catalog=QJsonDocument::fromJson(manifest.readAll()).object();QJsonObject model;
            for(auto entry:catalog.value("models").toArray())if(entry.toObject().value("id").toString()==id)model=entry.toObject();
            const auto record=model.value("images").toArray().first().toObject();
            const QImage image(directory.filePath(record.value("file").toString()));QVERIFY(!image.isNull());
            AiOptions options;options.policy=qEnvironmentVariable("COMPOSITOR_AI_SELECTION_PROVIDER")=="dml"?AiProviderPolicy::DirectMLOnly:AiProviderPolicy::CpuOnly;
            EditorWindow w;auto p=page(w);p->document=Document::create(image.size());p->document.addImage("Image",image);p->canvas->refresh();
            p->aiSelection=createAiSelectionService(qEnvironmentVariable("COMPOSITOR_AI_SELECTION_MODEL_ROOT"),options);
            p->canvas->selectAll();const auto original=p->session.selection;const int count=p->history.count();
            QImage expected;
            if(id=="birefnet-lite") {
                expected=aiSubjectSelection(reference(directory,"logits",record.value("encoder_outputs").toObject().value("logits").toObject()),image);
                action(w,"Select Subject")->trigger();
            } else {
                auto prompt=record.value("cases").toArray().first().toObject();auto outputs=prompt.value("outputs").toObject();AiRunResult run;
                for(auto name:{"masks","iou_predictions","low_res_masks"})run.outputs.append(reference(directory,name,outputs.value(name).toObject()));
                expected=aiObjectSelection(run,image,id=="sam2"?AiImageKind::SAM2:AiImageKind::MobileSAM).mask;
                action(w,"Select Object…")->trigger();auto dialog=w.findChild<QDialog *>("aiObjectDialog");
                dialog->findChild<QComboBox *>("aiObjectModel")->setCurrentIndex(id=="sam2"?0:1);
                auto point=prompt.value("points").toArray().first().toArray();emit p->canvas->aiPointPicked({point[0].toDouble(),point[1].toDouble()},false);
                QTRY_VERIFY_WITH_TIMEOUT(ok(dialog)->isEnabled(),60000);ok(dialog)->click();
            }
            QCOMPARE(p->history.count(),count+1);QVERIFY(!p->isModified());
            const double overlap=iou(p->session.selection,expected);QVERIFY2(overlap>=.995,qPrintable(QString("Editor selection IoU %1").arg(overlap)));
            p->history.undo();QCOMPARE(p->session.selection,original);p->history.redo();QVERIFY(iou(p->session.selection,expected)>=.995);
            QFile report("artifacts/ai2-editor-"+qEnvironmentVariable("COMPOSITOR_AI_SELECTION_PROVIDER","cpu")+"-"+id+".json");
            QVERIFY(report.open(QIODevice::WriteOnly));report.write(QJsonDocument(QJsonObject{{"model",id},{"iou",overlap},{"undo_redo",true},{"content_dirty",p->isModified()}}).toJson());
        }catch(const std::exception &error){QFAIL(error.what());}
    }
    void realModelSelection_data() {
        QTest::addColumn<QString>("id");QTest::addColumn<QString>("filename");
        for(auto id:{"birefnet-lite","sam2","mobilesam"})
            for(auto image:{"truck.png","cars.png","groceries.png"})
                QTest::newRow((QString(id)+"/"+image).toUtf8().constData())<<QString(id)<<QString(image);
    }
    void realModelSelection() {
        if(!qEnvironmentVariableIsSet("COMPOSITOR_AI_SELECTION_MODEL_ROOT")) QSKIP("Opt-in real AI selection verification");
        QFETCH(QString,id);QFETCH(QString,filename);
        try {
        QDir directory(qEnvironmentVariable("COMPOSITOR_AI_SELECTION_REFERENCES"));
        QFile manifest(directory.filePath("references.json"));QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto catalog=QJsonDocument::fromJson(manifest.readAll()).object();
        QJsonObject model,record;
        for(auto entry:catalog.value("models").toArray()) if(entry.toObject().value("id").toString()==id) model=entry.toObject();
        for(auto entry:model.value("images").toArray()) if(entry.toObject().value("file").toString()==filename) record=entry.toObject();
        QVERIFY(!record.isEmpty());const QImage image(directory.filePath(filename));QVERIFY(!image.isNull());
        AiOptions options;options.policy=qEnvironmentVariable("COMPOSITOR_AI_SELECTION_PROVIDER")=="dml"?AiProviderPolicy::DirectMLOnly:AiProviderPolicy::CpuOnly;
        auto service=createAiSelectionService(qEnvironmentVariable("COMPOSITOR_AI_SELECTION_MODEL_ROOT"),options);
        auto cancel=std::make_shared<AiCancellation>();QJsonArray checks;
        if(id=="birefnet-lite") {
            auto expected=aiSubjectSelection(reference(directory,"logits",record.value("encoder_outputs").toObject().value("logits").toObject()),image);
            auto actual=service->subject(image,cancel);const double overlap=iou(actual.mask,expected);
            QVERIFY2(overlap>=.995,qPrintable(QString("Subject IoU %1").arg(overlap)));
            checks.append(QJsonObject{{"case","subject"},{"iou",overlap},{"backend",actual.backend},{"milliseconds",actual.milliseconds}});
            actual.mask.save("artifacts/ai2-"+id+"-"+filename);
        } else {
            const auto kind=id=="sam2"?AiImageKind::SAM2:AiImageKind::MobileSAM;
            for(auto entry:record.value("cases").toArray()) {
                const auto prompt=entry.toObject();if(prompt.value("refine").toBool())continue;
                QVector<QPointF> points;QVector<int> labels;
                for(auto point:prompt.value("points").toArray()) { auto p=point.toArray();points.append({p[0].toDouble(),p[1].toDouble()}); }
                for(auto label:prompt.value("labels").toArray())labels.append(label.toInt());
                AiRunResult expectedRun;
                auto output=prompt.value("outputs").toObject();
                for(auto name:{"masks","iou_predictions","low_res_masks"}) expectedRun.outputs.append(reference(directory,name,output.value(name).toObject()));
                const auto expected=aiObjectSelection(expectedRun,image,kind);
                const auto actual=service->object(image,id,points,labels,cancel);const double overlap=iou(actual.mask,expected.mask);
                QCOMPARE(actual.candidate,expected.candidate);QVERIFY2(overlap>=.995,qPrintable(QString("Object IoU %1").arg(overlap)));
                QCOMPARE(actual.encoderRuns,quint64(1));
                checks.append(QJsonObject{{"case",prompt.value("name")},{"iou",overlap},{"candidate",actual.candidate},{"encoder_runs",double(actual.encoderRuns)},
                    {"backend",actual.backend},{"milliseconds",actual.milliseconds}});
                actual.mask.save("artifacts/ai2-"+id+"-"+prompt.value("name").toString()+"-"+filename);
            }
        }
        QFile report("artifacts/ai2-real-"+qEnvironmentVariable("COMPOSITOR_AI_SELECTION_PROVIDER","cpu")+"-"+id+"-"+filename+".json");
        QVERIFY(report.open(QIODevice::WriteOnly));report.write(QJsonDocument(QJsonObject{{"model",id},{"image",filename},{"checks",checks}}).toJson());
        } catch(const std::exception &error) { QFAIL(error.what()); }
    }
};
int main(int argc,char **argv) {
    QApplication app(argc,argv);QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QCoreApplication::setOrganizationName("CompositorTests");QCoreApplication::setApplicationName("AiSelectionTests");
    SelectionTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "ai_selection_tests.moc"
