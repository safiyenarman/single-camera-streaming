#include <QCoreApplication>
#include "cameraclient.h"
int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    cameraclient client;
    return QCoreApplication::exec();
}
