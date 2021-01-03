#ifndef TESTHELPER_H
#define TESTHELPER_H

#include "folder.h"
#include "creds/httpcredentials.h"

using namespace OCC;

class HttpCredentialsTest : public HttpCredentials {
public:
    HttpCredentialsTest(const QString& user, const QString& password)
    : HttpCredentials(user, password)
    {}

    void askFromUser() Q_DECL_OVERRIDE {

    }
};

static inline FolderDefinition folderDefinition(const QString &path) {
    FolderDefinition d;
    d.localPath = path;
    d.targetPath = path;
    d.alias = path;
    return d;
}

#endif // TESTHELPER_H
