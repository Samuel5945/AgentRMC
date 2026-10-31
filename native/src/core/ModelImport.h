#pragma once

#include <QString>

class QWidget;
class QMimeData;

struct ModelImportResult {
    bool ok = false;
    QString backend;  // sensevoice / qwen3
    QString error;
};

// 从 .zip / .tar.bz2 等包解出模型，装到 exe 同目录 models\<名称>。
// parent 用来挂进度窗；返回装的是哪个后端。
ModelImportResult importModelPackage(QWidget *parent, const QString &file);

// 拖拽进来的文件里挑一个像模型包的；没有就返回空。
QString modelPackageFromMime(const QMimeData *mime);
