//  Natron
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef ABOUTWINDOW_H
#define ABOUTWINDOW_H

#include <QDialog>

class QTextBrowser;
class QLabel;
class QTabWidget;
class QVBoxLayout;
class QHBoxLayout;
class Button;

class AboutWindow : public QDialog
{
    
    QVBoxLayout* _mainLayout;
    QLabel* _iconLabel;
    QTabWidget* _tabWidget;
    
    QTextBrowser* _aboutText;
    //QTextBrowser* _libsText;
    QTextBrowser* _teamText;
    QTextBrowser* _licenseText;
    
    QWidget* _buttonContainer;
    QHBoxLayout* _buttonLayout;
    Button* _closeButton;
    
public:
    
    AboutWindow(QWidget* parent = 0);
    
    virtual ~AboutWindow() {}
};

#endif // ABOUTWINDOW_H