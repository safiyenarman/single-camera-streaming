#include "serverwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    serverwindow w;
    //w.show();
    w.showMaximized();
    return a.exec();
}
