#include "openmvplugin.h"

#include "app/app_version.h"

namespace OpenMV {
namespace Internal {

OpenMVPlugin::OpenMVPlugin() : IPlugin()
{
    qRegisterMetaType<OpenMVPluginSerialPortCommand>("OpenMVPluginSerialPortCommand");
    qRegisterMetaType<OpenMVPluginSerialPortCommandResult>("OpenMVPluginSerialPortCommandResult");

    m_ioport = new OpenMVPluginSerialPort(this);
    m_iodevice = new OpenMVPluginIO(m_ioport, this);

    m_frameSizeDumpTimer.start();
    m_getScriptRunningTimer.start();
    m_getTxBufferTimer.start();

    m_timer.start();
    m_queue = QQueue<qint64>();

    m_working = false;
    m_connected = false;
    m_running = false;
    m_major = int();
    m_minor = int();
    m_patch = int();
    m_portName = QString();
    m_portPath = QString();

    m_errorFilterRegex = QRegularExpression(QStringLiteral(
        "  File \"(.+?)\", line (\\d+).*?\n"
        "(?!Exception: IDE interrupt)(.+?:.+?)\n"));
    m_errorFilterString = QString();

    QTimer *timer = new QTimer(this);

    connect(timer, &QTimer::timeout,
            this, &OpenMVPlugin::processEvents);

    timer->start(1);
}

bool OpenMVPlugin::initialize(const QStringList &arguments, QString *errorMessage)
{
    Q_UNUSED(arguments)
    Q_UNUSED(errorMessage)

    QSplashScreen *splashScreen = new QSplashScreen(QPixmap(QStringLiteral(SPLASH_PATH)));

    connect(Core::ICore::instance(), &Core::ICore::coreOpened,
            splashScreen, &QSplashScreen::deleteLater);

    splashScreen->show();

    return true;
}

void OpenMVPlugin::extensionsInitialized()
{
    QApplication::setApplicationDisplayName(tr("OpenMV IDE"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(ICON_PATH)));

    connect(Core::ActionManager::command(Core::Constants::NEW)->action(), &QAction::triggered, this, [this] {
        Core::EditorManager::cutForwardNavigationHistory();
        Core::EditorManager::addCurrentPositionToNavigationHistory();
        QString titlePattern = tr("untitled_$.py");
        TextEditor::BaseTextEditor *editor = qobject_cast<TextEditor::BaseTextEditor *>(Core::EditorManager::openEditorWithContents(Core::Constants::K_DEFAULT_TEXT_EDITOR_ID, &titlePattern,
            tr("# Untitled - By: %L1 - %L2\n"
               "\n"
               "import sensor\n"
               "\n"
               "sensor.reset()\n"
               "sensor.set_pixformat(sensor.RGB565)\n"
               "sensor.set_framesize(sensor.QVGA)\n"
               "sensor.skip_frames()\n"
               "\n"
               "while(True):\n"
               "    img = sensor.snapshot()\n").
            arg(Utils::Environment::systemEnvironment().userName()).arg(QDate::currentDate().toString()).toLatin1()));
        if(editor)
        {
            editor->editorWidget()->configureGenericHighlighter();
            Core::EditorManager::activateEditor(editor);
        }
        else
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                QObject::tr("New File"),
                QObject::tr("Can't open the new file!"));
        }
    });

    Core::ActionContainer *filesMenu = Core::ActionManager::actionContainer(Core::Constants::M_FILE);
    Core::ActionContainer *examplesMenu = Core::ActionManager::createMenu(Core::Id("OpenMV.Examples"));
    filesMenu->addMenu(Core::ActionManager::actionContainer(Core::Constants::M_FILE_RECENTFILES), examplesMenu, Core::Constants::G_FILE_OPEN);
    examplesMenu->menu()->setTitle(tr("Examples"));
    examplesMenu->setOnAllDisabledBehavior(Core::ActionContainer::Show);
    connect(filesMenu->menu(), &QMenu::aboutToShow, this, [this, examplesMenu] {
        examplesMenu->menu()->clear();
        QMap<QString, QAction *> actions = aboutToShowExamplesRecursive(Core::ICore::userResourcePath() + QStringLiteral("/examples"), examplesMenu->menu());
        examplesMenu->menu()->addActions(actions.values());
        examplesMenu->menu()->setDisabled(actions.values().isEmpty());
    });

    ///////////////////////////////////////////////////////////////////////////

    Core::ActionContainer *toolsMenu = Core::ActionManager::actionContainer(Core::Constants::M_TOOLS);
    Core::ActionContainer *helpMenu = Core::ActionManager::actionContainer(Core::Constants::M_HELP);

    QAction *bootloaderCommand = new QAction(tr("Run Bootloader"), this);
    m_bootloaderCommand = Core::ActionManager::registerAction(bootloaderCommand, Core::Id("OpenMV.Bootloader"));
    toolsMenu->addAction(m_bootloaderCommand);
    bootloaderCommand->setEnabled(true);
    connect(bootloaderCommand, &QAction::triggered, this, &OpenMVPlugin::bootloaderClicked);
    toolsMenu->addSeparator();

    QAction *saveCommand = new QAction(tr("Save open script to OpenMV Cam"), this);
    m_saveCommand = Core::ActionManager::registerAction(saveCommand, Core::Id("OpenMV.Save"));
    toolsMenu->addAction(m_saveCommand);
    saveCommand->setEnabled(false);
    connect(saveCommand, &QAction::triggered, this, &OpenMVPlugin::saveScript);

    QAction *resetCommand = new QAction(tr("Reset OpenMV Cam"), this);
    m_resetCommand = Core::ActionManager::registerAction(resetCommand, Core::Id("OpenMV.Reset"));
    toolsMenu->addAction(m_resetCommand);
    resetCommand->setEnabled(false);
    connect(resetCommand, &QAction::triggered, this, [this] {disconnectClicked(true);});

    toolsMenu->addSeparator();
    m_machineVisionToolsMenu = Core::ActionManager::createMenu(Core::Id("OpenMV.MachineVision"));
    m_machineVisionToolsMenu->menu()->setTitle(tr("Machine Vision"));
    toolsMenu->addMenu(m_machineVisionToolsMenu);

    QAction *keypointsEditorCommand = new QAction(tr("Keypoints Editor"), this);
    m_keypointsEditorCommand = Core::ActionManager::registerAction(keypointsEditorCommand, Core::Id("OpenMV.KeypointsEditor"));
    m_machineVisionToolsMenu->addAction(m_keypointsEditorCommand);
    connect(keypointsEditorCommand, &QAction::triggered, this, &OpenMVPlugin::openKeypointsEditor);

    QAction *docsCommand = new QAction(tr("OpenMV Docs"), this);
    m_docsCommand = Core::ActionManager::registerAction(docsCommand, Core::Id("OpenMV.Docs"));
    helpMenu->addAction(m_docsCommand, Core::Constants::G_HELP_SUPPORT);
    docsCommand->setEnabled(true);
    connect(docsCommand, &QAction::triggered, this, [this] {
        QUrl url = QUrl::fromLocalFile(Core::ICore::userResourcePath() + QStringLiteral("/html/index.html"));

        if(!QDesktopServices::openUrl(url))
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                                  QString(),
                                  tr("Failed to open: \"%L1\"").arg(url.toString()));
        }
    });

    QAction *forumsCommand = new QAction(tr("OpenMV Forums"), this);
    m_forumsCommand = Core::ActionManager::registerAction(forumsCommand, Core::Id("OpenMV.Forums"));
    helpMenu->addAction(m_forumsCommand, Core::Constants::G_HELP_SUPPORT);
    forumsCommand->setEnabled(true);
    connect(forumsCommand, &QAction::triggered, this, [this] {
        QUrl url = QUrl(QStringLiteral("http://forums.openmv.io/"));

        if(!QDesktopServices::openUrl(url))
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                                  QString(),
                                  tr("Failed to open: \"%L1\"").arg(url.toString()));
        }
    });

    QAction *pinoutAction = new QAction(
         Utils::HostOsInfo::isMacHost() ? tr("About OpenMV Cam") : tr("About OpenMV Cam..."), this);
    pinoutAction->setMenuRole(QAction::ApplicationSpecificRole);
    m_pinoutCommand = Core::ActionManager::registerAction(pinoutAction, Core::Id("OpenMV.Pinout"));
    helpMenu->addAction(m_pinoutCommand, Core::Constants::G_HELP_ABOUT);
    pinoutAction->setEnabled(true);
    connect(pinoutAction, &QAction::triggered, this, [this] {
        QUrl url = QUrl::fromLocalFile(Core::ICore::userResourcePath() + QStringLiteral("/html/_images/pinout.png"));

        if(!QDesktopServices::openUrl(url))
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                                  QString(),
                                  tr("Failed to open: \"%L1\"").arg(url.toString()));
        }
    });

    QAction *aboutAction = new QAction(QIcon::fromTheme(QStringLiteral("help-about")),
        Utils::HostOsInfo::isMacHost() ? tr("About OpenMV IDE") : tr("About OpenMV IDE..."), this);
    aboutAction->setMenuRole(QAction::AboutRole);
    m_aboutCommand = Core::ActionManager::registerAction(aboutAction, Core::Id("OpenMV.About"));
    helpMenu->addAction(m_aboutCommand, Core::Constants::G_HELP_ABOUT);
    aboutAction->setEnabled(true);
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(Core::ICore::dialogParent(), tr("About OpenMV IDE"), tr(
        "<p><b>About OpenMV IDE %L1</b></p>"
        "<p>By: Ibrahim Abdelkader & Kwabena W. Agyeman</p>"
        "<p><b>GNU GENERAL PUBLIC LICENSE</b></p>"
        "<p>Copyright (C) %L2 %L3</p>"
        "<p>This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the <a href=\"http://github.com/openmv/qt-creator/raw/master/LICENSE.GPL3-EXCEPT\">GNU General Public License</a> for more details.</p>"
        "<p><b>Questions or Comments?</b></p>"
        "<p>Contact us at <a href=\"mailto:openmv@openmv.io\">openmv@openmv.io</a>.</p>"
        ).arg(QLatin1String(Core::Constants::OMV_IDE_VERSION_LONG)).arg(QLatin1String(Core::Constants::OMV_IDE_YEAR)).arg(QLatin1String(Core::Constants::OMV_IDE_AUTHOR)));
    });

    ///////////////////////////////////////////////////////////////////////////

    m_connectCommand =
        Core::ActionManager::registerAction(new QAction(QIcon(QStringLiteral(CONNECT_PATH)),
        tr("Connect"), this), Core::Id("OpenMV.Connect"));
    m_connectCommand->setDefaultKeySequence(tr("Ctrl+E"));
    m_connectCommand->action()->setEnabled(true);
    m_connectCommand->action()->setVisible(true);
    connect(m_connectCommand->action(), &QAction::triggered, this, [this] {connectClicked();});

    m_disconnectCommand =
        Core::ActionManager::registerAction(new QAction(QIcon(QStringLiteral(DISCONNECT_PATH)),
        tr("Disconnect"), this), Core::Id("OpenMV.Disconnect"));
    m_disconnectCommand->setDefaultKeySequence(tr("Ctrl+E"));
    m_disconnectCommand->action()->setEnabled(false);
    m_disconnectCommand->action()->setVisible(false);
    connect(m_disconnectCommand->action(), &QAction::triggered, this, [this] {disconnectClicked();});

    m_startCommand =
        Core::ActionManager::registerAction(new QAction(QIcon(QStringLiteral(START_PATH)),
        tr("Start (run script)"), this), Core::Id("OpenMV.Start"));
    m_startCommand->setDefaultKeySequence(tr("Ctrl+R"));
    m_startCommand->action()->setEnabled(false);
    m_startCommand->action()->setVisible(true);
    connect(m_startCommand->action(), &QAction::triggered, this, &OpenMVPlugin::startClicked);
    connect(Core::EditorManager::instance(), &Core::EditorManager::currentEditorChanged, [this] (Core::IEditor *editor) {
        if(m_connected)
        {
            m_saveCommand->action()->setEnabled((!m_portPath.isEmpty()) && (editor ? (editor->document() ? (!editor->document()->contents().isEmpty()) : false) : false));
            m_startCommand->action()->setEnabled((!m_running) && (editor ? (editor->document() ? (!editor->document()->contents().isEmpty()) : false) : false));
            m_startCommand->action()->setVisible(!m_running);
            m_stopCommand->action()->setEnabled(m_running);
            m_stopCommand->action()->setVisible(m_running);
        }
    });

    m_stopCommand =
        Core::ActionManager::registerAction(new QAction(QIcon(QStringLiteral(STOP_PATH)),
        tr("Stop (halt script)"), this), Core::Id("OpenMV.Stop"));
    m_stopCommand->setDefaultKeySequence(tr("Ctrl+R"));
    m_stopCommand->action()->setEnabled(false);
    m_stopCommand->action()->setVisible(false);
    connect(m_stopCommand->action(), &QAction::triggered, this, &OpenMVPlugin::stopClicked);
    connect(m_iodevice, &OpenMVPluginIO::scriptRunning, this, [this] (bool running) {
        if(m_connected)
        {
            Core::IEditor *editor = Core::EditorManager::currentEditor();
            m_saveCommand->action()->setEnabled((!m_portPath.isEmpty()) && (editor ? (editor->document() ? (!editor->document()->contents().isEmpty()) : false) : false));
            m_startCommand->action()->setEnabled((!running) && (editor ? (editor->document() ? (!editor->document()->contents().isEmpty()) : false) : false));
            m_startCommand->action()->setVisible(!running);
            m_stopCommand->action()->setEnabled(running);
            m_stopCommand->action()->setVisible(running);
            m_running = running;
        }
    });

    ///////////////////////////////////////////////////////////////////////////

    QMainWindow *mainWindow = q_check_ptr(qobject_cast<QMainWindow *>(Core::ICore::mainWindow()));
    Core::Internal::FancyTabWidget *widget = q_check_ptr(qobject_cast<Core::Internal::FancyTabWidget *>(mainWindow->centralWidget()));

    Core::Internal::FancyActionBar *actionBar0 = new Core::Internal::FancyActionBar(widget);
    widget->insertCornerWidget(0, actionBar0);

    actionBar0->insertAction(0, Core::ActionManager::command(Core::Constants::NEW)->action());
    actionBar0->insertAction(1, Core::ActionManager::command(Core::Constants::OPEN)->action());
    actionBar0->insertAction(2, Core::ActionManager::command(Core::Constants::SAVE)->action());

    actionBar0->setProperty("no_separator", true);
    actionBar0->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    Core::Internal::FancyActionBar *actionBar1 = new Core::Internal::FancyActionBar(widget);
    widget->insertCornerWidget(1, actionBar1);

    actionBar1->insertAction(0, Core::ActionManager::command(Core::Constants::UNDO)->action());
    actionBar1->insertAction(1, Core::ActionManager::command(Core::Constants::REDO)->action());
    actionBar1->insertAction(2, Core::ActionManager::command(Core::Constants::CUT)->action());
    actionBar1->insertAction(3, Core::ActionManager::command(Core::Constants::COPY)->action());
    actionBar1->insertAction(4, Core::ActionManager::command(Core::Constants::PASTE)->action());

    actionBar1->setProperty("no_separator", false);
    actionBar1->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    Core::Internal::FancyActionBar *actionBar2 = new Core::Internal::FancyActionBar(widget);
    widget->insertCornerWidget(2, actionBar2);

    actionBar2->insertAction(0, m_connectCommand->action());
    actionBar2->insertAction(1, m_disconnectCommand->action());
    actionBar2->insertAction(2, m_startCommand->action());
    actionBar2->insertAction(3, m_stopCommand->action());

    actionBar2->setProperty("no_separator", false);
    actionBar2->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    ///////////////////////////////////////////////////////////////////////////

    Utils::StyledBar *styledBar0 = new Utils::StyledBar;
    QHBoxLayout *styledBar0Layout = new QHBoxLayout;
    styledBar0Layout->setMargin(0);
    styledBar0Layout->setSpacing(0);
    styledBar0Layout->addSpacing(4);
    styledBar0Layout->addWidget(new QLabel(tr("Frame Buffer")));
    styledBar0Layout->addSpacing(6);
    styledBar0->setLayout(styledBar0Layout);

    m_zoom = new QToolButton;
    m_zoom->setText(tr("Zoom"));
    m_zoom->setToolTip(tr("Zoom to fit"));
    m_zoom->setCheckable(true);
    m_zoom->setChecked(false);
    styledBar0Layout->addWidget(m_zoom);

    m_jpgCompress = new QToolButton;
    m_jpgCompress->setText(tr("JPG"));
    m_jpgCompress->setToolTip(tr("JPEG compress the Frame Buffer for higher performance"));
    m_jpgCompress->setCheckable(true);
    m_jpgCompress->setChecked(true);
    ///// Disable JPEG Compress /////
    m_jpgCompress->setVisible(false);
    styledBar0Layout->addWidget(m_jpgCompress);
    connect(m_jpgCompress, &QToolButton::clicked, this, [this] {
        if(m_connected)
        {
            if(!m_working)
            {
                m_iodevice->jpegEnable(m_jpgCompress->isChecked());
            }
            else
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("JPG"),
                    tr("Busy... please wait..."));
            }
        }
    });

    m_disableFrameBuffer = new QToolButton;
    m_disableFrameBuffer->setText(tr("Disable"));
    m_disableFrameBuffer->setToolTip(tr("Disable the Frame Buffer for maximum performance"));
    m_disableFrameBuffer->setCheckable(true);
    m_disableFrameBuffer->setChecked(false);
    styledBar0Layout->addWidget(m_disableFrameBuffer);
    connect(m_disableFrameBuffer, &QToolButton::clicked, this, [this] {
        if(m_connected)
        {
            if(!m_working)
            {
                m_iodevice->fbEnable(!m_disableFrameBuffer->isChecked());
            }
            else
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Disable"),
                    tr("Busy... please wait..."));
            }
        }
    });

    m_frameBuffer = new OpenMVPluginFB;
    QWidget *tempWidget0 = new QWidget;
    QVBoxLayout *tempLayout0 = new QVBoxLayout;
    tempLayout0->setMargin(0);
    tempLayout0->setSpacing(0);
    tempLayout0->addWidget(styledBar0);
    tempLayout0->addWidget(m_frameBuffer);
    tempWidget0->setLayout(tempLayout0);
    connect(m_zoom, &QToolButton::toggled, m_frameBuffer, &OpenMVPluginFB::enableFitInView);
    connect(m_iodevice, &OpenMVPluginIO::frameBufferData, m_frameBuffer, &OpenMVPluginFB::frameBufferData);
    connect(m_frameBuffer, &OpenMVPluginFB::saveImage, this, &OpenMVPlugin::saveImage);
    connect(m_frameBuffer, &OpenMVPluginFB::saveTemplate, this, &OpenMVPlugin::saveTemplate);
    connect(m_frameBuffer, &OpenMVPluginFB::saveDescriptor, this, &OpenMVPlugin::saveDescriptor);

    Utils::StyledBar *styledBar1 = new Utils::StyledBar;
    QHBoxLayout *styledBar1Layout = new QHBoxLayout;
    styledBar1Layout->setMargin(0);
    styledBar1Layout->setSpacing(0);
    styledBar1Layout->addSpacing(4);
    styledBar1Layout->addWidget(new QLabel(tr("Histogram")));
    styledBar1Layout->addSpacing(6);
    styledBar1->setLayout(styledBar1Layout);

    m_histogramColorSpace = new QComboBox;
    m_histogramColorSpace->setProperty("hideborder", true);
    m_histogramColorSpace->setProperty("drawleftborder", false);
    m_histogramColorSpace->insertItem(RGB_COLOR_SPACE, tr("RGB Color Space"));
    m_histogramColorSpace->insertItem(GRAYSCALE_COLOR_SPACE, tr("Grayscale Color Space"));
    m_histogramColorSpace->insertItem(LAB_COLOR_SPACE, tr("LAB Color Space"));
    m_histogramColorSpace->insertItem(YUV_COLOR_SPACE, tr("YUV Color Space"));
    m_histogramColorSpace->setCurrentIndex(RGB_COLOR_SPACE);
    m_histogramColorSpace->setToolTip(tr("Use Grayscale/LAB for color tracking"));
    styledBar1Layout->addWidget(m_histogramColorSpace);

    m_histogram = new OpenMVPluginHistogram;
    QWidget *tempWidget1 = new QWidget;
    QVBoxLayout *tempLayout1 = new QVBoxLayout;
    tempLayout1->setMargin(0);
    tempLayout1->setSpacing(0);
    tempLayout1->addWidget(styledBar1);
    tempLayout1->addWidget(m_histogram);
    tempWidget1->setLayout(tempLayout1);
    connect(m_histogramColorSpace, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), m_histogram, &OpenMVPluginHistogram::colorSpaceChanged);
    connect(m_frameBuffer, &OpenMVPluginFB::pixmapUpdate, m_histogram, &OpenMVPluginHistogram::pixmapUpdate);

    m_hsplitter = widget->m_hsplitter;
    m_vsplitter = widget->m_vsplitter;
    m_vsplitter->insertWidget(0, tempWidget0);
    m_vsplitter->insertWidget(1, tempWidget1);
    m_vsplitter->setStretchFactor(0, 0);
    m_vsplitter->setStretchFactor(1, 1);

    connect(m_iodevice, &OpenMVPluginIO::printData, Core::MessageManager::instance(), &Core::MessageManager::printData);
    connect(m_iodevice, &OpenMVPluginIO::printData, this, &OpenMVPlugin::errorFilter);
    connect(m_iodevice, &OpenMVPluginIO::frameBufferData, this, [this] {
        m_queue.push_back(m_timer.restart());

        if(m_queue.size() > FPS_AVERAGE_BUFFER_DEPTH)
        {
            m_queue.pop_front();
        }

        qint64 average = 0;

        for(int i = 0; i < m_queue.size(); i++)
        {
            average += m_queue.at(i);
        }

        average /= m_queue.size();

        m_fpsLabel->setText(tr("FPS: %L1").arg(average ? (1000 / double(average)) : 0, 5, 'f', 1));
    });

    ///////////////////////////////////////////////////////////////////////////

    m_versionButton = new QToolButton;
    m_versionButton->setText(tr("Firmware Version:"));
    m_versionButton->setToolTip(tr("Camera firmware version"));
    m_versionButton->setCheckable(false);
    m_versionButton->setDisabled(true);
    Core::ICore::statusBar()->addPermanentWidget(m_versionButton);
    Core::ICore::statusBar()->addPermanentWidget(new QLabel());
    connect(m_versionButton, &QToolButton::clicked, this, &OpenMVPlugin::updateCam);

    m_portLabel = new QLabel(tr("Serial Port:"));
    m_portLabel->setToolTip(tr("Camera serial port"));
    m_portLabel->setDisabled(true);
    Core::ICore::statusBar()->addPermanentWidget(m_portLabel);
    Core::ICore::statusBar()->addPermanentWidget(new QLabel());

    m_pathButton = new QToolButton;
    m_pathButton->setText(tr("Drive:"));
    m_pathButton->setToolTip(tr("Drive associated with port"));
    m_pathButton->setCheckable(false);
    m_pathButton->setDisabled(true);
    Core::ICore::statusBar()->addPermanentWidget(m_pathButton);
    Core::ICore::statusBar()->addPermanentWidget(new QLabel());
    connect(m_pathButton, &QToolButton::clicked, this, &OpenMVPlugin::setPortPath);

    m_fpsLabel = new QLabel(tr("FPS:"));
    m_fpsLabel->setToolTip(tr("May be different from camera FPS"));
    m_fpsLabel->setDisabled(true);
    m_fpsLabel->setMinimumWidth(m_fpsLabel->fontMetrics().width(QStringLiteral("FPS: 000.000")));
    Core::ICore::statusBar()->addPermanentWidget(m_fpsLabel);

    ///////////////////////////////////////////////////////////////////////////

    QSettings *settings = ExtensionSystem::PluginManager::settings();
    settings->beginGroup(QStringLiteral(SETTINGS_GROUP));
    Core::EditorManager::restoreState(
        settings->value(QStringLiteral(EDITOR_MANAGER_STATE)).toByteArray());
    m_hsplitter->restoreState(
        settings->value(QStringLiteral(HSPLITTER_STATE)).toByteArray());
    m_vsplitter->restoreState(
        settings->value(QStringLiteral(VSPLITTER_STATE)).toByteArray());
    m_zoom->setChecked(
        settings->value(QStringLiteral(ZOOM_STATE), m_zoom->isChecked()).toBool());
    m_jpgCompress->setChecked(
        settings->value(QStringLiteral(JPG_COMPRESS_STATE), m_jpgCompress->isChecked()).toBool());
    m_disableFrameBuffer->setChecked(
        settings->value(QStringLiteral(DISABLE_FRAME_BUFFER_STATE), m_disableFrameBuffer->isChecked()).toBool());
    m_histogramColorSpace->setCurrentIndex(
        settings->value(QStringLiteral(HISTOGRAM_COLOR_SPACE_STATE), m_histogramColorSpace->currentIndex()).toInt());
    settings->endGroup();

    connect(Core::ICore::instance(), &Core::ICore::saveSettingsRequested, this, [this] {
        QSettings *settings = ExtensionSystem::PluginManager::settings();
        settings->beginGroup(QStringLiteral(SETTINGS_GROUP));
        settings->setValue(QStringLiteral(EDITOR_MANAGER_STATE),
            Core::EditorManager::saveState());
        settings->setValue(QStringLiteral(HSPLITTER_STATE),
            m_hsplitter->saveState());
        settings->setValue(QStringLiteral(VSPLITTER_STATE),
            m_vsplitter->saveState());
        settings->setValue(QStringLiteral(ZOOM_STATE),
            m_zoom->isChecked());
        settings->setValue(QStringLiteral(JPG_COMPRESS_STATE),
            m_jpgCompress->isChecked());
        settings->setValue(QStringLiteral(DISABLE_FRAME_BUFFER_STATE),
            m_disableFrameBuffer->isChecked());
        settings->setValue(QStringLiteral(HISTOGRAM_COLOR_SPACE_STATE),
            m_histogramColorSpace->currentIndex());
        settings->endGroup();
    });

    ///////////////////////////////////////////////////////////////////////////

    settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

    int major = settings->value(QStringLiteral(RESOURCES_MAJOR), 0).toInt();
    int minor = settings->value(QStringLiteral(RESOURCES_MINOR), 0).toInt();
    int patch = settings->value(QStringLiteral(RESOURCES_PATCH), 0).toInt();

    if((major < OMV_IDE_VERSION_MAJOR)
    || ((major == OMV_IDE_VERSION_MAJOR) && (minor < OMV_IDE_VERSION_MINOR))
    || ((major == OMV_IDE_VERSION_MAJOR) && (minor == OMV_IDE_VERSION_MINOR) && (patch < OMV_IDE_VERSION_RELEASE)))
    {
        settings->setValue(QStringLiteral(RESOURCES_MAJOR), 0);
        settings->setValue(QStringLiteral(RESOURCES_MINOR), 0);
        settings->setValue(QStringLiteral(RESOURCES_PATCH), 0);
        settings->sync();

        bool ok = true;

        QString error;

        if(!Utils::FileUtils::removeRecursively(Utils::FileName::fromString(Core::ICore::userResourcePath()), &error))
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                QString(),
                error + tr("\n\nPlease close any programs that are viewing/editing OpenMV IDE's application data and then restart OpenMV IDE!"));

            QApplication::quit();
            ok = false;
        }
        else
        {
            QStringList list = QStringList() << QStringLiteral("examples") << QStringLiteral("firmware") << QStringLiteral("html");

            foreach(const QString &dir, list)
            {
                QString error;

                if(!Utils::FileUtils::copyRecursively(Utils::FileName::fromString(Core::ICore::resourcePath() + QChar::fromLatin1('/') + dir),
                                                      Utils::FileName::fromString(Core::ICore::userResourcePath() + QChar::fromLatin1('/') + dir),
                                                      &error))
                {
                    QMessageBox::critical(Core::ICore::dialogParent(),
                        QString(),
                        error + tr("\n\nPlease close any programs that are viewing/editing OpenMV IDE's application data and then restart OpenMV IDE!"));

                    QApplication::quit();
                    ok = false;
                    break;
                }
            }
        }

        if(ok)
        {
            settings->setValue(QStringLiteral(RESOURCES_MAJOR), OMV_IDE_VERSION_MAJOR);
            settings->setValue(QStringLiteral(RESOURCES_MINOR), OMV_IDE_VERSION_MINOR);
            settings->setValue(QStringLiteral(RESOURCES_PATCH), OMV_IDE_VERSION_RELEASE);
            settings->sync();
        }
    }

    settings->endGroup();

    ///////////////////////////////////////////////////////////////////////////

    Core::IEditor *editor = Core::EditorManager::currentEditor();

    if(editor ? (editor->document() ? editor->document()->contents().isEmpty() : true) : true)
    {
        QString filePath = Core::ICore::userResourcePath() + QStringLiteral("/examples/01-Basics/helloworld.py");

        QFile file(filePath);

        if(file.open(QIODevice::ReadOnly))
        {
            QByteArray data = file.readAll();

            if((file.error() == QFile::NoError) && (!data.isEmpty()))
            {
                Core::EditorManager::cutForwardNavigationHistory();
                Core::EditorManager::addCurrentPositionToNavigationHistory();

                QString titlePattern = QFileInfo(filePath).baseName().simplified() + QStringLiteral("_$.") + QFileInfo(filePath).completeSuffix();
                TextEditor::BaseTextEditor *editor = qobject_cast<TextEditor::BaseTextEditor *>(Core::EditorManager::openEditorWithContents(Core::Constants::K_DEFAULT_TEXT_EDITOR_ID, &titlePattern, data));

                if(editor)
                {
                    editor->editorWidget()->configureGenericHighlighter();
                    Core::EditorManager::activateEditor(editor);
                }
            }
        }
    }

    ///////////////////////////////////////////////////////////////////////////

    QLoggingCategory::setFilterRules(QStringLiteral("qt.network.ssl.warning=false")); // http://stackoverflow.com/questions/26361145/qsslsocket-error-when-ssl-is-not-used

    connect(Core::ICore::instance(), &Core::ICore::coreOpened, this, [this] {

        QNetworkAccessManager *manager = new QNetworkAccessManager(this);

        connect(manager, &QNetworkAccessManager::finished, this, [this] (QNetworkReply *reply) {

            QByteArray data = reply->readAll();

            if((reply->error() == QNetworkReply::NoError) && (!data.isEmpty()))
            {
                QRegularExpressionMatch match = QRegularExpression(QStringLiteral("(\\d+)\\.(\\d+)\\.(\\d+)")).match(QString::fromLatin1(data));

                int major = match.captured(1).toInt();
                int minor = match.captured(2).toInt();
                int patch = match.captured(3).toInt();

                if((OMV_IDE_VERSION_MAJOR < major)
                || ((OMV_IDE_VERSION_MAJOR == major) && (OMV_IDE_VERSION_MINOR < minor))
                || ((OMV_IDE_VERSION_MAJOR == major) && (OMV_IDE_VERSION_MINOR == minor) && (OMV_IDE_VERSION_RELEASE < patch)))
                {
                    QMessageBox box(QMessageBox::Information, tr("Update Available"), tr("A new version of OpenMV IDE (%L1.%L2.%L3) is available for download.").arg(major).arg(minor).arg(patch), QMessageBox::Cancel, Core::ICore::dialogParent(),
                        Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                        (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));
                    QPushButton *button = box.addButton(tr("Download"), QMessageBox::AcceptRole);
                    box.setDefaultButton(button);
                    box.setEscapeButton(QMessageBox::Cancel);
                    box.exec();

                    if(box.clickedButton() == button)
                    {
                        QUrl url = QUrl(QStringLiteral("http://openmv.io/pages/download"));

                        if(!QDesktopServices::openUrl(url))
                        {
                            QMessageBox::critical(Core::ICore::dialogParent(),
                                                  QString(),
                                                  tr("Failed to open: \"%L1\"").arg(url.toString()));
                        }
                    }
                    else
                    {
                        QTimer::singleShot(0, this, &OpenMVPlugin::packageUpdate);
                    }
                }
                else
                {
                    QTimer::singleShot(0, this, &OpenMVPlugin::packageUpdate);
                }
            }
            else
            {
                QTimer::singleShot(0, this, &OpenMVPlugin::packageUpdate);
            }

            reply->deleteLater();
        });

        QNetworkRequest request = QNetworkRequest(QUrl(QStringLiteral("http://upload.openmv.io/openmv-ide-version.txt")));
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
        QNetworkReply *reply = manager->get(request);

        if(reply)
        {
            connect(reply, &QNetworkReply::sslErrors, reply, static_cast<void (QNetworkReply::*)(void)>(&QNetworkReply::ignoreSslErrors));
        }
        else
        {
            QTimer::singleShot(0, this, &OpenMVPlugin::packageUpdate);
        }
    });
}

ExtensionSystem::IPlugin::ShutdownFlag OpenMVPlugin::aboutToShutdown()
{
    if(!m_connected)
    {
        if(!m_working)
        {
            return ExtensionSystem::IPlugin::SynchronousShutdown;
        }
        else
        {
            connect(this, &OpenMVPlugin::workingDone, this, [this] {disconnectClicked();});
            connect(this, &OpenMVPlugin::disconnectDone, this, &OpenMVPlugin::asynchronousShutdownFinished);
            return ExtensionSystem::IPlugin::AsynchronousShutdown;
        }
    }
    else
    {
        if(!m_working)
        {
            connect(this, &OpenMVPlugin::disconnectDone, this, &OpenMVPlugin::asynchronousShutdownFinished);
            QTimer::singleShot(0, this, [this] {disconnectClicked();});
            return ExtensionSystem::IPlugin::AsynchronousShutdown;
        }
        else
        {
            connect(this, &OpenMVPlugin::workingDone, this, [this] {disconnectClicked();});
            connect(this, &OpenMVPlugin::disconnectDone, this, &OpenMVPlugin::asynchronousShutdownFinished);
            return ExtensionSystem::IPlugin::AsynchronousShutdown;
        }
    }
}

static bool removeRecursively(const Utils::FileName &path, QString *error)
{
    return Utils::FileUtils::removeRecursively(path, error);
}

static bool removeRecursivelyWrapper(const Utils::FileName &path, QString *error)
{
    QEventLoop loop;
    QFutureWatcher<bool> watcher;
    QObject::connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(removeRecursively, path, error));
    loop.exec();
    return watcher.result();
}

static bool extractAll(QByteArray *data, const QString &path)
{
    QBuffer buffer(data);
    QZipReader reader(&buffer);
    return reader.extractAll(path);
}

static bool extractAllWrapper(QByteArray *data, const QString &path)
{
    QEventLoop loop;
    QFutureWatcher<bool> watcher;
    QObject::connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(extractAll, data, path));
    loop.exec();
    return watcher.result();
}

void OpenMVPlugin::packageUpdate()
{
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);

    connect(manager, &QNetworkAccessManager::finished, this, [this] (QNetworkReply *reply) {

        QByteArray data = reply->readAll();

        if((reply->error() == QNetworkReply::NoError) && (!data.isEmpty()))
        {
            QRegularExpressionMatch match = QRegularExpression(QStringLiteral("(\\d+)\\.(\\d+)\\.(\\d+)")).match(QString::fromLatin1(data));

            int new_major = match.captured(1).toInt();
            int new_minor = match.captured(2).toInt();
            int new_patch = match.captured(3).toInt();

            QSettings *settings = ExtensionSystem::PluginManager::settings();
            settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

            int old_major = settings->value(QStringLiteral(RESOURCES_MAJOR)).toInt();
            int old_minor = settings->value(QStringLiteral(RESOURCES_MINOR)).toInt();
            int old_patch = settings->value(QStringLiteral(RESOURCES_PATCH)).toInt();

            settings->endGroup();

            if((old_major < new_major)
            || ((old_major == new_major) && (old_minor < new_minor))
            || ((old_major == new_major) && (old_minor == new_minor) && (old_patch < new_patch)))
            {
                QMessageBox box(QMessageBox::Information, tr("Update Available"), tr("New OpenMV IDE reources are available (e.g. examples, firmware, documentation, etc.)."), QMessageBox::Cancel, Core::ICore::dialogParent(),
                    Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                    (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));
                QPushButton *button = box.addButton(tr("Install"), QMessageBox::AcceptRole);
                box.setDefaultButton(button);
                box.setEscapeButton(QMessageBox::Cancel);
                box.exec();

                if(box.clickedButton() == button)
                {
                    QProgressDialog *dialog = new QProgressDialog(tr("Installing..."), tr("Cancel"), 0, 0, Core::ICore::dialogParent(),
                        Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
                        (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowType(0)));
                    dialog->setWindowModality(Qt::ApplicationModal);
                    dialog->setAttribute(Qt::WA_ShowWithoutActivating);
                    dialog->setCancelButton(Q_NULLPTR);

                    QNetworkAccessManager *manager2 = new QNetworkAccessManager(this);

                    connect(manager2, &QNetworkAccessManager::finished, this, [this, new_major, new_minor, new_patch, dialog] (QNetworkReply *reply2) {

                        QByteArray data2 = reply2->readAll();

                        if((reply2->error() == QNetworkReply::NoError) && (!data2.isEmpty()))
                        {
                            QSettings *settings2 = ExtensionSystem::PluginManager::settings();
                            settings2->beginGroup(QStringLiteral(SETTINGS_GROUP));

                            settings2->setValue(QStringLiteral(RESOURCES_MAJOR), 0);
                            settings2->setValue(QStringLiteral(RESOURCES_MINOR), 0);
                            settings2->setValue(QStringLiteral(RESOURCES_PATCH), 0);
                            settings2->sync();

                            bool ok = true;

                            QString error;

                            if(!removeRecursivelyWrapper(Utils::FileName::fromString(Core::ICore::userResourcePath()), &error))
                            {
                                QMessageBox::critical(Core::ICore::dialogParent(),
                                    QString(),
                                    error + tr("\n\nPlease close any programs that are viewing/editing OpenMV IDE's application data and then restart OpenMV IDE!"));

                                QApplication::quit();
                                ok = false;
                            }
                            else
                            {
                                if(!extractAllWrapper(&data2, Core::ICore::userResourcePath()))
                                {
                                    QMessageBox::critical(Core::ICore::dialogParent(),
                                        QString(),
                                        tr("Please close any programs that are viewing/editing OpenMV IDE's application data and then restart OpenMV IDE!"));

                                    QApplication::quit();
                                    ok = false;
                                }
                            }

                            if(ok)
                            {
                                settings2->setValue(QStringLiteral(RESOURCES_MAJOR), new_major);
                                settings2->setValue(QStringLiteral(RESOURCES_MINOR), new_minor);
                                settings2->setValue(QStringLiteral(RESOURCES_PATCH), new_patch);
                                settings2->sync();

                                QMessageBox::information(Core::ICore::dialogParent(),
                                    QString(),
                                    tr("Installation Sucessful! Please restart OpenMV IDE."));

                                QApplication::quit();
                            }

                            settings2->endGroup();
                        }
                        else if(reply2->error() != QNetworkReply::NoError)
                        {
                            QMessageBox::critical(Core::ICore::dialogParent(),
                                tr("Package Update"),
                                tr("Error: %L1!").arg(reply2->errorString()));
                        }
                        else
                        {
                            QMessageBox::critical(Core::ICore::dialogParent(),
                                tr("Package Update"),
                                tr("Cannot open the resources file \"%L1\"!").arg(reply2->request().url().toString()));
                        }

                        reply2->deleteLater();

                        delete dialog;
                    });

                    QNetworkRequest request2 = QNetworkRequest(QUrl(QStringLiteral("http://upload.openmv.io/openmv-ide-resources-%L1.%L2.%L3/openmv-ide-resources-%L1.%L2.%L3.zip").arg(new_major).arg(new_minor).arg(new_patch)));
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
                    request2.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
                    QNetworkReply *reply2 = manager2->get(request2);

                    if(reply2)
                    {
                        connect(reply2, &QNetworkReply::sslErrors, reply2, static_cast<void (QNetworkReply::*)(void)>(&QNetworkReply::ignoreSslErrors));
                        dialog->show();
                    }
                    else
                    {
                        QMessageBox::critical(Core::ICore::dialogParent(),
                            tr("Package Update"),
                            tr("Network request failed \"%L1\"!").arg(request2.url().toString()));
                    }
                }
            }
        }

        reply->deleteLater();
    });

    QNetworkRequest request = QNetworkRequest(QUrl(QStringLiteral("http://upload.openmv.io/openmv-ide-resources-version.txt")));
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
    QNetworkReply *reply = manager->get(request);

    if(reply)
    {
        connect(reply, &QNetworkReply::sslErrors, reply, static_cast<void (QNetworkReply::*)(void)>(&QNetworkReply::ignoreSslErrors));
    }
}

void OpenMVPlugin::bootloaderClicked()
{
    QDialog *dialog = new QDialog(Core::ICore::dialogParent(),
        Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
        (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));
    dialog->setWindowTitle(tr("Bootloader"));
    QFormLayout *layout = new QFormLayout(dialog);
    layout->setVerticalSpacing(0);

    QSettings *settings = ExtensionSystem::PluginManager::settings();
    settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

    Utils::PathChooser *pathChooser = new Utils::PathChooser();
    pathChooser->setExpectedKind(Utils::PathChooser::File);
    pathChooser->setPromptDialogTitle(tr("Firmware Path"));
    pathChooser->setPromptDialogFilter(tr("Firmware Binary (*.bin *.dfu)"));
    pathChooser->setFileName(Utils::FileName::fromString(settings->value(QStringLiteral(LAST_FIRMWARE_PATH), QDir::homePath()).toString()));
    layout->addRow(tr("Firmware Path"), pathChooser);
    layout->addItem(new QSpacerItem(0, 6));

    QCheckBox *checkBox = new QCheckBox(tr("Erase internal file system"));
    checkBox->setChecked(settings->value(QStringLiteral(LAST_FLASH_FS_ERASE_STATE), false).toBool());
    checkBox->setVisible(!pathChooser->path().endsWith(QStringLiteral(".dfu"), Qt::CaseInsensitive));
    layout->addRow(checkBox);
    QCheckBox *checkBox2 = new QCheckBox(tr("Erase internal file system"));
    checkBox2->setChecked(true);
    checkBox2->setEnabled(false);
    checkBox2->setVisible(pathChooser->path().endsWith(QStringLiteral(".dfu"), Qt::CaseInsensitive));
    layout->addRow(checkBox2);
    layout->addItem(new QSpacerItem(0, 6));

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *run = new QPushButton(tr("Run"));
    run->setEnabled(pathChooser->isValid());
    box->addButton(run, QDialogButtonBox::AcceptRole);
    layout->addRow(box);

    connect(box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(pathChooser, &Utils::PathChooser::validChanged, run, &QPushButton::setEnabled);
    connect(pathChooser, &Utils::PathChooser::pathChanged, this, [this, dialog, checkBox, checkBox2] (const QString &path) {
        if(path.endsWith(QStringLiteral(".dfu"), Qt::CaseInsensitive))
        {
            checkBox->setVisible(false);
            checkBox2->setVisible(true);
        }
        else
        {
            checkBox->setVisible(true);
            checkBox2->setVisible(false);

        }
        dialog->adjustSize();
    });

    if(dialog->exec() == QDialog::Accepted)
    {
        QString forceFirmwarePath = pathChooser->path();
        bool flashFSErase = checkBox->isChecked();

        if(QFileInfo(forceFirmwarePath).isFile())
        {
            settings->setValue(QStringLiteral(LAST_FIRMWARE_PATH), forceFirmwarePath);
            settings->setValue(QStringLiteral(LAST_FLASH_FS_ERASE_STATE), flashFSErase);
            settings->endGroup();
            delete dialog;

            connectClicked(true, forceFirmwarePath, (flashFSErase || forceFirmwarePath.endsWith(QStringLiteral(".dfu"), Qt::CaseInsensitive)) ? QMessageBox::Yes : QMessageBox::No);
        }
        else
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                tr("Bootloader"),
                tr("\"%L1\" is not a file!").arg(forceFirmwarePath));

            settings->endGroup();
            delete dialog;
        }
    }
    else
    {
        settings->endGroup();
        delete dialog;
    }
}

#define CONNECT_END() \
do { \
    m_working = false; \
    QTimer::singleShot(0, this, &OpenMVPlugin::workingDone); \
    return; \
} while(0)

#define RECONNECT_END() \
do { \
    m_working = false; \
    QTimer::singleShot(0, this, [this] {connectClicked();}); \
    return; \
} while(0)

#define CLOSE_CONNECT_END() \
do { \
    QEventLoop m_loop; \
    connect(m_iodevice, &OpenMVPluginIO::closeResponse, &m_loop, &QEventLoop::quit); \
    m_iodevice->close(); \
    m_loop.exec(); \
    m_working = false; \
    QTimer::singleShot(0, this, &OpenMVPlugin::workingDone); \
    return; \
} while(0)

#define CLOSE_RECONNECT_END() \
do { \
    QEventLoop m_loop; \
    connect(m_iodevice, &OpenMVPluginIO::closeResponse, &m_loop, &QEventLoop::quit); \
    m_iodevice->close(); \
    m_loop.exec(); \
    m_working = false; \
    QTimer::singleShot(0, this, [this] {connectClicked();}); \
    return; \
} while(0)

void OpenMVPlugin::connectClicked(bool forceBootloader, QString forceFirmwarePath, int forceFlashFSErase)
{
    if(!m_working)
    {
        m_working = true;

        QStringList stringList;

        foreach(QSerialPortInfo port, QSerialPortInfo::availablePorts())
        {
            if(port.hasVendorIdentifier() && (port.vendorIdentifier() == OPENMVCAM_VID)
            && port.hasProductIdentifier() && (port.productIdentifier() == OPENMVCAM_PID))
            {
                stringList.append(port.portName());
            }
        }

        if(Utils::HostOsInfo::isMacHost())
        {
            stringList = stringList.filter(QStringLiteral("cu"), Qt::CaseInsensitive);
        }

        QSettings *settings = ExtensionSystem::PluginManager::settings();
        settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

        QString selectedPort;
        bool forceBootloaderBricked = false;
        QString firmwarePath = forceFirmwarePath;

        if(stringList.isEmpty())
        {
            if(forceBootloader)
            {
                forceBootloaderBricked = true;
            }
            else
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Connect"),
                    tr("No OpenMV Cams found!"));

                QFile boards(Core::ICore::userResourcePath() + QStringLiteral("/firmware/boards.txt"));

                if(boards.open(QIODevice::ReadOnly))
                {
                    QMap<QString, QString> mappings;

                    forever
                    {
                        QByteArray data = boards.readLine();

                        if((boards.error() == QFile::NoError) && (!data.isEmpty()))
                        {
                            QRegularExpressionMatch mapping = QRegularExpression(QStringLiteral("(\\S+)\\s+(\\S+)\\s+(\\S+)")).match(QString::fromLatin1(data));
                            mappings.insert(mapping.captured(2).replace(QStringLiteral("_"), QStringLiteral(" ")), mapping.captured(3).replace(QStringLiteral("_"), QStringLiteral(" ")));
                        }
                        else
                        {
                            boards.close();
                            break;
                        }
                    }

                    if(!mappings.isEmpty())
                    {
                        if(QMessageBox::question(Core::ICore::dialogParent(),
                            tr("Connect"),
                            tr("Do you have an OpenMV Cam connected and is it bricked?"),
                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes)
                        == QMessageBox::Yes)
                        {
                            int index = mappings.keys().indexOf(settings->value(QStringLiteral(LAST_BOARD_TYPE_STATE)).toString());

                            bool ok;
                            QString temp = QInputDialog::getItem(Core::ICore::dialogParent(),
                                tr("Connect"), tr("Please select the board type"),
                                mappings.keys(), (index != -1) ? index : 0, false, &ok,
                                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));

                            if(ok)
                            {
                                settings->setValue(QStringLiteral(LAST_BOARD_TYPE_STATE), temp);

                                int answer = QMessageBox::question(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("Erase the internal file system?"),
                                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);

                                if((answer == QMessageBox::Yes) || (answer == QMessageBox::No))
                                {
                                    forceBootloader = true;
                                    forceFlashFSErase = answer;
                                    forceBootloaderBricked = true;
                                    firmwarePath = Core::ICore::userResourcePath() + QStringLiteral("/firmware/") + mappings.value(temp) + QStringLiteral("/firmware.bin");
                                }
                            }
                        }
                    }
                }
            }
        }
        else if(stringList.size() == 1)
        {
            selectedPort = stringList.first();
            settings->setValue(QStringLiteral(LAST_SERIAL_PORT_STATE), selectedPort);
        }
        else
        {
            int index = stringList.indexOf(settings->value(QStringLiteral(LAST_SERIAL_PORT_STATE)).toString());

            bool ok;
            QString temp = QInputDialog::getItem(Core::ICore::dialogParent(),
                tr("Connect"), tr("Please select a serial port"),
                stringList, (index != -1) ? index : 0, false, &ok,
                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));

            if(ok)
            {
                selectedPort = temp;
                settings->setValue(QStringLiteral(LAST_SERIAL_PORT_STATE), selectedPort);
            }
        }

        settings->endGroup();

        if((!forceBootloaderBricked) && selectedPort.isEmpty())
        {
            CONNECT_END();
        }

        // Open Port //////////////////////////////////////////////////////////

        if(!forceBootloaderBricked)
        {
            QString errorMessage2 = QString();
            QString *errorMessage2Ptr = &errorMessage2;

            QMetaObject::Connection conn = connect(m_ioport, &OpenMVPluginSerialPort::openResult,
                this, [this, errorMessage2Ptr] (const QString &errorMessage) {
                *errorMessage2Ptr = errorMessage;
            });

            QProgressDialog dialog(tr("Connecting..."), tr("Cancel"), 0, 0, Core::ICore::dialogParent(),
               Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
               (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowType(0)));
            dialog.setWindowModality(Qt::ApplicationModal);
            dialog.setAttribute(Qt::WA_ShowWithoutActivating);

            forever
            {
                QEventLoop loop;

                connect(m_ioport, &OpenMVPluginSerialPort::openResult,
                        &loop, &QEventLoop::quit);

                m_ioport->open(selectedPort);

                loop.exec();

                if(errorMessage2.isEmpty() || (Utils::HostOsInfo::isLinuxHost() && errorMessage2.contains(QStringLiteral("Permission Denied"), Qt::CaseInsensitive)))
                {
                    break;
                }

                dialog.show();

                QApplication::processEvents();

                if(dialog.wasCanceled())
                {
                    break;
                }
            }

            dialog.close();

            disconnect(conn);

            if(!errorMessage2.isEmpty())
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Connect"),
                    tr("Error: %L1!").arg(errorMessage2));

                if(Utils::HostOsInfo::isLinuxHost() && errorMessage2.contains(QStringLiteral("Permission Denied"), Qt::CaseInsensitive))
                {
                    QMessageBox::information(Core::ICore::dialogParent(),
                        tr("Connect"),
                        tr("Try doing:\n\nsudo adduser %L1 dialout\n\n...in a terminal and then restart your computer.").arg(Utils::Environment::systemEnvironment().userName()));
                }

                CONNECT_END();
            }
        }

        // Get Version ////////////////////////////////////////////////////////

        int major2 = int();
        int minor2 = int();
        int patch2 = int();

        if(!forceBootloaderBricked)
        {
            int *major2Ptr = &major2;
            int *minor2Ptr = &minor2;
            int *patch2Ptr = &patch2;

            QMetaObject::Connection conn = connect(m_iodevice, &OpenMVPluginIO::firmwareVersion,
                this, [this, major2Ptr, minor2Ptr, patch2Ptr] (int major, int minor, int patch) {
                *major2Ptr = major;
                *minor2Ptr = minor;
                *patch2Ptr = patch;
            });

            QEventLoop loop;

            connect(m_iodevice, &OpenMVPluginIO::firmwareVersion,
                    &loop, &QEventLoop::quit);

            m_iodevice->getFirmwareVersion();

            loop.exec();

            disconnect(conn);

            if((!major2) && (!minor2) && (!patch2))
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Connect"),
                    tr("Timeout error while getting firmware version!"));

                QMessageBox::warning(Core::ICore::dialogParent(),
                    tr("Connect"),
                    tr("Do not try to connect while the green light on your OpenMV Cam is on!"));

                if(QMessageBox::question(Core::ICore::dialogParent(),
                    tr("Connect"),
                    tr("Try to connect again?"),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes)
                == QMessageBox::Yes)
                {
                    CLOSE_RECONNECT_END();
                }

                CLOSE_CONNECT_END();
            }
            else if((major2 < 0) || (100 < major2) || (minor2 < 0) || (100 < minor2) || (patch2 < 0) || (100 < patch2))
            {
                CLOSE_RECONNECT_END();
            }
        }

        // Bootloader /////////////////////////////////////////////////////////

        if(forceBootloader)
        {
            if(!forceBootloaderBricked)
            {
                if(firmwarePath.isEmpty())
                {
                    if((major2 < OLD_API_MAJOR)
                    || ((major2 == OLD_API_MAJOR) && (minor2 < OLD_API_MINOR))
                    || ((major2 == OLD_API_MAJOR) && (minor2 == OLD_API_MINOR) && (patch2 < OLD_API_PATCH)))
                    {
                        firmwarePath = Core::ICore::userResourcePath() + QStringLiteral("/firmware/") + QStringLiteral(OLD_API_BOARD) + QStringLiteral("/firmware.bin");
                    }
                    else
                    {
                        QString arch2 = QString();
                        QString *arch2Ptr = &arch2;

                        QMetaObject::Connection conn = connect(m_iodevice, &OpenMVPluginIO::archString,
                            this, [this, arch2Ptr] (const QString &arch) {
                            *arch2Ptr = arch;
                        });

                        QEventLoop loop;

                        connect(m_iodevice, &OpenMVPluginIO::archString,
                                &loop, &QEventLoop::quit);

                        m_iodevice->getArchString();

                        loop.exec();

                        disconnect(conn);

                        if(!arch2.isEmpty())
                        {
                            QFile boards(Core::ICore::userResourcePath() + QStringLiteral("/firmware/boards.txt"));

                            if(boards.open(QIODevice::ReadOnly))
                            {
                                QMap<QString, QString> mappings;

                                forever
                                {
                                    QByteArray data = boards.readLine();

                                    if((boards.error() == QFile::NoError) && (!data.isEmpty()))
                                    {
                                        QRegularExpressionMatch mapping = QRegularExpression(QStringLiteral("(\\S+)\\s+(\\S+)\\s+(\\S+)")).match(QString::fromLatin1(data));
                                        mappings.insert(mapping.captured(1).replace(QStringLiteral("_"), QStringLiteral(" ")), mapping.captured(3).replace(QStringLiteral("_"), QStringLiteral(" ")));
                                    }
                                    else
                                    {
                                        boards.close();
                                        break;
                                    }
                                }

                                QString value = mappings.value(arch2.simplified().replace(QStringLiteral("_"), QStringLiteral(" ")));

                                if(!value.isEmpty())
                                {
                                    firmwarePath = Core::ICore::userResourcePath() + QStringLiteral("/firmware/") + value + QStringLiteral("/firmware.bin");
                                }
                                else
                                {
                                    QMessageBox::critical(Core::ICore::dialogParent(),
                                        tr("Connect"),
                                        tr("Unsupported board architecture!"));

                                    CLOSE_CONNECT_END();
                                }
                            }
                            else
                            {
                                QMessageBox::critical(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("Error: %L1!").arg(boards.errorString()));

                                CLOSE_CONNECT_END();
                            }
                        }
                        else
                        {
                            QMessageBox::critical(Core::ICore::dialogParent(),
                                tr("Connect"),
                                tr("Timeout error while getting board architecture!"));

                            CLOSE_CONNECT_END();
                        }
                    }
                }

                if(firmwarePath.endsWith(QStringLiteral(".dfu"), Qt::CaseInsensitive))
                {
                    QEventLoop loop;

                    connect(m_iodevice, &OpenMVPluginIO::closeResponse,
                            &loop, &QEventLoop::quit);

                    m_iodevice->close();

                    loop.exec();
                }
            }

            // BIN Bootloader /////////////////////////////////////////////////

            while(firmwarePath.endsWith(QStringLiteral(".bin"), Qt::CaseInsensitive))
            {
                QFile file(firmwarePath);

                if(file.open(QIODevice::ReadOnly))
                {
                    QByteArray data = file.readAll();

                    if((file.error() == QFile::NoError) && (!data.isEmpty()))
                    {
                        file.close();

                        QList<QByteArray> dataChunks;

                        for(int i = 0; i < data.size(); i += FLASH_WRITE_CHUNK_SIZE)
                        {
                            dataChunks.append(data.mid(i, qMin(FLASH_WRITE_CHUNK_SIZE, data.size() - i)));
                        }

                        if(dataChunks.last().size() % FLASH_WRITE_CHUNK_SIZE)
                        {
                            dataChunks.last().append(QByteArray(FLASH_WRITE_CHUNK_SIZE - dataChunks.last().size(), 255));
                        }

                        // Start Bootloader ///////////////////////////////////
                        {
                            bool done2 = bool(), loopExit = false, done22 = false;
                            bool *done2Ptr = &done2, *loopExitPtr = &loopExit, *done2Ptr2 = &done22;

                            QMetaObject::Connection conn = connect(m_ioport, &OpenMVPluginSerialPort::bootloaderStartResponse,
                                this, [this, done2Ptr, loopExitPtr] (bool done) {
                                *done2Ptr = done;
                                *loopExitPtr = true;
                            });

                            QMetaObject::Connection conn2 = connect(m_ioport, &OpenMVPluginSerialPort::bootloaderStopResponse,
                                this, [this, done2Ptr2] {
                                *done2Ptr2 = true;
                            });

                            QProgressDialog dialog(forceBootloaderBricked ? tr("Disconnect your OpenMV Cam and then reconnect it...") : tr("Connecting..."), tr("Cancel"), 0, 0, Core::ICore::dialogParent(),
                                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
                                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowType(0)));
                            dialog.setWindowModality(Qt::ApplicationModal);
                            dialog.setAttribute(Qt::WA_ShowWithoutActivating);
                            dialog.show();

                            connect(&dialog, &QProgressDialog::canceled,
                                    m_ioport, &OpenMVPluginSerialPort::bootloaderStop);

                            QEventLoop loop, loop0, loop1;

                            connect(m_ioport, &OpenMVPluginSerialPort::bootloaderStartResponse,
                                    &loop, &QEventLoop::quit);

                            connect(m_ioport, &OpenMVPluginSerialPort::bootloaderStopResponse,
                                    &loop0, &QEventLoop::quit);

                            connect(m_ioport, &OpenMVPluginSerialPort::bootloaderResetResponse,
                                    &loop1, &QEventLoop::quit);

                            m_ioport->bootloaderStart(selectedPort);

                            // NOT loop.exec();
                            while(!loopExit)
                            {
                                QSerialPortInfo::availablePorts();
                                QApplication::processEvents();
                                // Keep updating the list of available serial
                                // ports for the non-gui serial thread.
                            }

                            dialog.close();

                            if(!done22)
                            {
                                loop0.exec();
                            }

                            m_ioport->bootloaderReset();

                            loop1.exec();

                            disconnect(conn);

                            disconnect(conn2);

                            if(!done2)
                            {
                                QMessageBox::critical(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("Unable to connect to your OpenMV Cam's normal bootloader!"));

                                if(forceFirmwarePath.isEmpty() && QMessageBox::question(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("OpenMV IDE can still try to upgrade your OpenMV Cam using your OpenMV Cam's DFU Bootloader.\n\n"
                                       "Continue?"),
                                    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok)
                                == QMessageBox::Ok)
                                {
                                    firmwarePath = QFileInfo(firmwarePath).path() + QStringLiteral("/openmv.dfu");
                                    break;
                                }

                                CONNECT_END();
                            }
                        }

                        // Erase Flash ////////////////////////////////////////
                        {
                            int flash_start = forceFlashFSErase ? FLASH_SECTOR_ALL_START : FLASH_SECTOR_START;
                            int flash_end = forceFlashFSErase ? FLASH_SECTOR_ALL_END : FLASH_SECTOR_END;

                            bool ok2 = bool();
                            bool *ok2Ptr = &ok2;

                            QMetaObject::Connection conn2 = connect(m_iodevice, &OpenMVPluginIO::flashEraseDone,
                                this, [this, ok2Ptr] (bool ok) {
                                *ok2Ptr = ok;
                            });

                            QProgressDialog dialog(tr("Erasing..."), tr("Cancel"), flash_start, flash_end, Core::ICore::dialogParent(),
                                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
                                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowType(0)));
                            dialog.setWindowModality(Qt::ApplicationModal);
                            dialog.setAttribute(Qt::WA_ShowWithoutActivating);
                            dialog.setCancelButton(Q_NULLPTR);
                            dialog.show();

                            for(int i = flash_start; i <= flash_end; i++)
                            {
                                QEventLoop loop0, loop1;

                                connect(m_iodevice, &OpenMVPluginIO::flashEraseDone,
                                        &loop0, &QEventLoop::quit);

                                m_iodevice->flashErase(i);

                                loop0.exec();

                                if(!ok2)
                                {
                                    break;
                                }

                                QTimer::singleShot(FLASH_ERASE_DELAY, &loop1, &QEventLoop::quit);

                                loop1.exec();

                                dialog.setValue(i);
                            }

                            dialog.close();

                            disconnect(conn2);

                            if(!ok2)
                            {
                                QMessageBox::critical(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("Timeout Error!"));

                                CLOSE_CONNECT_END();
                            }
                        }

                        // Program Flash //////////////////////////////////////
                        {
                            bool ok2 = bool();
                            bool *ok2Ptr = &ok2;

                            QMetaObject::Connection conn2 = connect(m_iodevice, &OpenMVPluginIO::flashWriteDone,
                                this, [this, ok2Ptr] (bool ok) {
                                *ok2Ptr = ok;
                            });

                            QProgressDialog dialog(tr("Programming..."), tr("Cancel"), 0, dataChunks.size() - 1, Core::ICore::dialogParent(),
                                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
                                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowType(0)));
                            dialog.setWindowModality(Qt::ApplicationModal);
                            dialog.setAttribute(Qt::WA_ShowWithoutActivating);
                            dialog.setCancelButton(Q_NULLPTR);
                            dialog.show();

                            for(int i = 0; i < dataChunks.size(); i++)
                            {
                                QEventLoop loop0, loop1;

                                connect(m_iodevice, &OpenMVPluginIO::flashWriteDone,
                                        &loop0, &QEventLoop::quit);

                                m_iodevice->flashWrite(dataChunks.at(i));

                                loop0.exec();

                                if(!ok2)
                                {
                                    break;
                                }

                                QTimer::singleShot(FLASH_WRITE_DELAY, &loop1, &QEventLoop::quit);

                                loop1.exec();

                                dialog.setValue(i);
                            }

                            dialog.close();

                            disconnect(conn2);

                            if(!ok2)
                            {
                                QMessageBox::critical(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("Timeout Error!"));

                                CLOSE_CONNECT_END();
                            }
                        }

                        // Reset Bootloader ///////////////////////////////////
                        {
                            QProgressDialog dialog(tr("Programming..."), tr("Cancel"), 0, 0, Core::ICore::dialogParent(),
                                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
                                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowType(0)));
                            dialog.setWindowModality(Qt::ApplicationModal);
                            dialog.setAttribute(Qt::WA_ShowWithoutActivating);
                            dialog.setCancelButton(Q_NULLPTR);
                            dialog.show();

                            QEventLoop loop;

                            connect(m_iodevice, &OpenMVPluginIO::closeResponse,
                                    &loop, &QEventLoop::quit);

                            m_iodevice->bootloaderReset();
                            m_iodevice->close();

                            loop.exec();

                            dialog.close();

                            QMessageBox::information(Core::ICore::dialogParent(),
                                tr("Connect"),
                                tr("Done upgrading your OpenMV Cam's firmware!\n\n"
                                   "Click the Ok button after your OpenMV Cam has enumerated and finished running its built-in self test (blue led blinking - this takes a while)."));

                            RECONNECT_END();
                        }
                    }
                    else if(file.error() != QFile::NoError)
                    {
                        QMessageBox::critical(Core::ICore::dialogParent(),
                            tr("Connect"),
                            tr("Error: %L1!").arg(file.errorString()));

                        if(forceBootloaderBricked)
                        {
                            CONNECT_END();
                        }
                        else
                        {
                            CLOSE_CONNECT_END();
                        }
                    }
                    else
                    {
                        QMessageBox::critical(Core::ICore::dialogParent(),
                            tr("Connect"),
                            tr("The firmware file is empty!"));

                        if(forceBootloaderBricked)
                        {
                            CONNECT_END();
                        }
                        else
                        {
                            CLOSE_CONNECT_END();
                        }
                    }
                }
                else
                {
                    QMessageBox::critical(Core::ICore::dialogParent(),
                        tr("Connect"),
                        tr("Error: %L1!").arg(file.errorString()));

                    if(forceBootloaderBricked)
                    {
                        CONNECT_END();
                    }
                    else
                    {
                        CLOSE_CONNECT_END();
                    }
                }
            }

            // DFU Bootloader /////////////////////////////////////////////////

            if(firmwarePath.endsWith(QStringLiteral(".dfu"), Qt::CaseInsensitive))
            {
                if(forceFlashFSErase || (QMessageBox::warning(Core::ICore::dialogParent(),
                    tr("Connect"),
                    tr("DFU update erases your OpenMV Cam's internal flash file system.\n\n"
                       "Backup your data before continuing!"),
                    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok)
                == QMessageBox::Ok))
                {
                    if(QMessageBox::information(Core::ICore::dialogParent(),
                        tr("Connect"),
                        tr("Disconnect your OpenMV Cam from your computer, add a jumper wire between the BOOT and RST pins, and then reconnect your OpenMV Cam to your computer.\n\n"
                           "Click the Ok button after your OpenMV Cam's DFU Bootloader has enumerated."),
                        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok)
                    == QMessageBox::Ok)
                    {
                        QProgressDialog dialog(tr("Reprogramming...\n\n(may take up to 5 minutes)"), tr("Cancel"), 0, 0, Core::ICore::dialogParent(),
                            Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::CustomizeWindowHint |
                            (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowType(0)));
                        dialog.setWindowModality(Qt::ApplicationModal);
                        dialog.setAttribute(Qt::WA_ShowWithoutActivating);
                        dialog.setCancelButton(Q_NULLPTR);
                        dialog.show();

                        QString command;
                        Utils::SynchronousProcess process;
                        Utils::SynchronousProcessResponse response;
                        process.setTimeoutS(300); // 5 minutes...
                        process.setProcessChannelMode(QProcess::MergedChannels);

                        if(Utils::HostOsInfo::isWindowsHost())
                        {
                            for(int i = 0; i < 10; i++) // try multiple times...
                            {
                                command = QDir::cleanPath(QDir::toNativeSeparators(Core::ICore::resourcePath() + QStringLiteral("/dfuse/DfuSeCommand.exe")));
                                response = process.run(command, QStringList()
                                    << QStringLiteral("-c")
                                    << QStringLiteral("-d")
                                    << QStringLiteral("--v")
                                    << QStringLiteral("--o")
                                    << QStringLiteral("--fn")
                                    << QDir::cleanPath(QDir::toNativeSeparators(firmwarePath)));

                                if(response.result == Utils::SynchronousProcessResponse::Finished)
                                {
                                    break;
                                }
                                else
                                {
                                    QApplication::processEvents();
                                }
                            }
                        }
                        else
                        {
                            for(int i = 0; i < 10; i++) // try multiple times...
                            {
                                command = QDir::cleanPath(QDir::toNativeSeparators(Core::ICore::resourcePath() + QStringLiteral("/pydfu/pydfu.py")));
                                response = process.run(QStringLiteral("python"), QStringList()
                                    << command
                                    << QStringLiteral("-u")
                                    << QDir::cleanPath(QDir::toNativeSeparators(firmwarePath)));

                                if(response.result == Utils::SynchronousProcessResponse::Finished)
                                {
                                    break;
                                }
                                else
                                {
                                    QApplication::processEvents();
                                }
                            }
                        }

                        if(response.result == Utils::SynchronousProcessResponse::Finished)
                        {
                            QMessageBox::information(Core::ICore::dialogParent(),
                                tr("Connect"),
                                tr("DFU firmware update complete!\n\n") +
                                (Utils::HostOsInfo::isWindowsHost() ? tr("Disconnect your OpenMV Cam from your computer, remove the jumper wire between the BOOT and RST pins, and then reconnect your OpenMV Cam to your computer.\n\n") : QString()) +
                                tr("Click the Ok button after your OpenMV Cam has enumerated and finished running its built-in self test (blue led blinking - this takes a while)."));

                            RECONNECT_END();
                        }
                        else
                        {
                            QMessageBox box(QMessageBox::Critical, tr("Connect"), tr("DFU firmware update failed!"), QMessageBox::Ok, Core::ICore::dialogParent(),
                                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));
                            box.setDetailedText(response.stdOut);
                            box.setInformativeText(response.exitMessage(command, process.timeoutS()));
                            box.setDefaultButton(QMessageBox::Ok);
                            box.setEscapeButton(QMessageBox::Cancel);
                            box.exec();

                            if(Utils::HostOsInfo::isMacHost())
                            {
                                QMessageBox::information(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("PyDFU requires the following libraries to be installed:\n\n"
                                       "MacPorts:\n"
                                       "    sudo port install libusb py-pip\n"
                                       "    sudo pip install pyusb\n\n"
                                       "HomeBrew:\n"
                                       "    sudo brew install libusb python\n"
                                       "    sudo pip install pyusb"));
                            }

                            if(Utils::HostOsInfo::isLinuxHost())
                            {
                                QMessageBox::information(Core::ICore::dialogParent(),
                                    tr("Connect"),
                                    tr("PyDFU requires the following libraries to be installed:\n\n"
                                       "    sudo apt-get install libusb-1.0 python-pip\n"
                                       "    sudo pip install pyusb"));
                            }
                        }
                    }
                }

                CONNECT_END();
            }
        }

        // Stopping ///////////////////////////////////////////////////////////

        m_iodevice->scriptStop();

        m_iodevice->jpegEnable(m_jpgCompress->isChecked());
        m_iodevice->fbEnable(!m_disableFrameBuffer->isChecked());

        Core::MessageManager::grayOutOldContent();

        ///////////////////////////////////////////////////////////////////////

        m_iodevice->getTimeout(); // clear

        m_frameSizeDumpTimer.restart();
        m_getScriptRunningTimer.restart();
        m_getTxBufferTimer.restart();

        m_timer.restart();
        m_queue.clear();
        m_connected = true;
        m_running = false;
        m_portName = selectedPort;
        m_portPath = QString();
        m_major = major2;
        m_minor = minor2;
        m_patch = patch2;
        m_errorFilterString = QString();

        m_bootloaderCommand->action()->setEnabled(false);
        m_saveCommand->action()->setEnabled(false);
        m_resetCommand->action()->setEnabled(true);
        m_connectCommand->action()->setEnabled(false);
        m_connectCommand->action()->setVisible(false);
        m_disconnectCommand->action()->setEnabled(true);
        m_disconnectCommand->action()->setVisible(true);
        Core::IEditor *editor = Core::EditorManager::currentEditor();
        m_startCommand->action()->setEnabled(editor ? (editor->document() ? (!editor->document()->contents().isEmpty()) : false) : false);
        m_startCommand->action()->setVisible(true);
        m_stopCommand->action()->setEnabled(false);
        m_stopCommand->action()->setVisible(false);

        m_versionButton->setEnabled(true);
        m_versionButton->setText(tr("Firmware Version: %L1.%L2.%L3").arg(major2).arg(minor2).arg(patch2));
        m_portLabel->setEnabled(true);
        m_portLabel->setText(tr("Serial Port: %L1").arg(m_portName));
        m_pathButton->setEnabled(true);
        m_pathButton->setText(tr("Drive:"));
        m_fpsLabel->setEnabled(true);
        m_fpsLabel->setText(tr("FPS: 0"));

        m_frameBuffer->enableSaveTemplate(false);
        m_frameBuffer->enableSaveDescriptor(false);

        // Check Version //////////////////////////////////////////////////////

        QFile file(Core::ICore::userResourcePath() + QStringLiteral("/firmware/firmware.txt"));

        if(file.open(QIODevice::ReadOnly))
        {
            QByteArray data = file.readAll();

            if((file.error() == QFile::NoError) && (!data.isEmpty()))
            {
                file.close();

                QRegularExpressionMatch match = QRegularExpression(QStringLiteral("(\\d+)\\.(\\d+)\\.(\\d+)")).match(QString::fromLatin1(data));

                if((major2 < match.captured(1).toInt())
                || ((major2 == match.captured(1).toInt()) && (minor2 < match.captured(2).toInt()))
                || ((major2 == match.captured(1).toInt()) && (minor2 == match.captured(2).toInt()) && (patch2 < match.captured(3).toInt())))
                {
                    m_versionButton->setText(m_versionButton->text().append(tr(" - [ out of date - click here to updgrade ]")));
                }
                else
                {
                    m_versionButton->setText(m_versionButton->text().append(tr(" - [ latest ]")));
                }
            }
        }

        ///////////////////////////////////////////////////////////////////////

        QTimer::singleShot(0, this, [this] { OpenMVPlugin::setPortPath(true); });

        CONNECT_END();
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Connect"),
            tr("Busy... please wait..."));
    }
}

void OpenMVPlugin::disconnectClicked(bool reset)
{
    if(m_connected)
    {
        if(!m_working)
        {
            m_working = true;

            // Stopping ///////////////////////////////////////////////////////
            {
                QEventLoop loop;

                connect(m_iodevice, &OpenMVPluginIO::closeResponse,
                        &loop, &QEventLoop::quit);

                if(reset)
                {
                    m_iodevice->sysReset();
                }
                else
                {
                    m_iodevice->scriptStop();
                }

                m_iodevice->close();

                loop.exec();
            }

            ///////////////////////////////////////////////////////////////////

            m_iodevice->getTimeout(); // clear

            m_frameSizeDumpTimer.restart();
            m_getScriptRunningTimer.restart();
            m_getTxBufferTimer.restart();

            m_timer.restart();
            m_queue.clear();
            m_connected = false;
            m_running = false;
            m_major = int();
            m_minor = int();
            m_patch = int();
            m_portName = QString();
            m_portPath = QString();
            m_errorFilterString = QString();

            m_bootloaderCommand->action()->setEnabled(true);
            m_saveCommand->action()->setEnabled(false);
            m_resetCommand->action()->setEnabled(false);
            m_connectCommand->action()->setEnabled(true);
            m_connectCommand->action()->setVisible(true);
            m_disconnectCommand->action()->setVisible(false);
            m_disconnectCommand->action()->setEnabled(false);
            m_startCommand->action()->setEnabled(false);
            m_startCommand->action()->setVisible(true);
            m_stopCommand->action()->setEnabled(false);
            m_stopCommand->action()->setVisible(false);

            m_versionButton->setDisabled(true);
            m_versionButton->setText(tr("Firmware Version:"));
            m_portLabel->setDisabled(true);
            m_portLabel->setText(tr("Serial Port:"));
            m_pathButton->setDisabled(true);
            m_pathButton->setText(tr("Drive:"));
            m_fpsLabel->setDisabled(true);
            m_fpsLabel->setText(tr("FPS:"));

            m_frameBuffer->enableSaveTemplate(false);
            m_frameBuffer->enableSaveDescriptor(false);

            ///////////////////////////////////////////////////////////////////

            m_working = false;
        }
        else
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                reset ? tr("Reset") : tr("Disconnect"),
                tr("Busy... please wait..."));
        }
    }

    QTimer::singleShot(0, this, &OpenMVPlugin::disconnectDone);
}

void OpenMVPlugin::startClicked()
{
    if(!m_working)
    {
        m_working = true;

        // Stopping ///////////////////////////////////////////////////////////
        {
            bool running2 = bool();
            bool *running2Ptr = &running2;

            QMetaObject::Connection conn = connect(m_iodevice, &OpenMVPluginIO::scriptRunning,
                this, [this, running2Ptr] (bool running) {
                *running2Ptr = running;
            });

            QEventLoop loop;

            connect(m_iodevice, &OpenMVPluginIO::scriptRunning,
                    &loop, &QEventLoop::quit);

            m_iodevice->getScriptRunning();

            loop.exec();

            disconnect(conn);

            if(running2)
            {
                m_iodevice->scriptStop();
            }
        }

        ///////////////////////////////////////////////////////////////////////

        m_iodevice->scriptExec(Core::EditorManager::currentEditor()->document()->contents());

        m_timer.restart();
        m_queue.clear();

        ///////////////////////////////////////////////////////////////////////

        m_working = false;

        QTimer::singleShot(0, this, &OpenMVPlugin::workingDone);
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Start"),
            tr("Busy... please wait..."));
    }
}

void OpenMVPlugin::stopClicked()
{
    if(!m_working)
    {
        m_working = true;

        // Stopping ///////////////////////////////////////////////////////////
        {
            bool running2 = bool();
            bool *running2Ptr = &running2;

            QMetaObject::Connection conn = connect(m_iodevice, &OpenMVPluginIO::scriptRunning,
                this, [this, running2Ptr] (bool running) {
                *running2Ptr = running;
            });

            QEventLoop loop;

            connect(m_iodevice, &OpenMVPluginIO::scriptRunning,
                    &loop, &QEventLoop::quit);

            m_iodevice->getScriptRunning();

            loop.exec();

            disconnect(conn);

            if(running2)
            {
                m_iodevice->scriptStop();
            }
        }

        ///////////////////////////////////////////////////////////////////////

        m_fpsLabel->setText(tr("FPS: 0"));

        ///////////////////////////////////////////////////////////////////////

        m_working = false;

        QTimer::singleShot(0, this, &OpenMVPlugin::workingDone);
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Stop"),
            tr("Busy... please wait..."));
    }
}

void OpenMVPlugin::processEvents()
{
    if((!m_working) && m_connected)
    {
        if(m_iodevice->getTimeout())
        {
            disconnectClicked(true);
        }
        else
        {
            if((!m_disableFrameBuffer->isChecked()) && (!m_iodevice->frameSizeDumpQueued()) && m_frameSizeDumpTimer.hasExpired(FRAME_SIZE_DUMP_SPACING))
            {
                m_frameSizeDumpTimer.restart();
                m_iodevice->frameSizeDump();
            }

            if((!m_iodevice->getScriptRunningQueued()) && m_getScriptRunningTimer.hasExpired(GET_SCRIPT_RUNNING_SPACING))
            {
                m_getScriptRunningTimer.restart();
                m_iodevice->getScriptRunning();

                if(m_portPath.isEmpty())
                {
                    setPortPath(true);
                }
            }

            if((!m_iodevice->getTxBufferQueued()) && m_getTxBufferTimer.hasExpired(GET_TX_BUFFER_SPACING))
            {
                m_getTxBufferTimer.restart();
                m_iodevice->getTxBuffer();
            }

            if(m_timer.hasExpired(FPS_TIMER_EXPIRATION_TIME))
            {
                m_fpsLabel->setText(tr("FPS: 0"));
            }
        }
    }
}

void OpenMVPlugin::errorFilter(const QByteArray &data)
{
    m_errorFilterString.append(Utils::SynchronousProcess::normalizeNewlines(QString::fromLatin1(data)));

    QRegularExpressionMatch match;
    int index = m_errorFilterString.indexOf(m_errorFilterRegex, 0, &match);

    if(index != -1)
    {
        QString fileName = match.captured(1);
        int lineNumber = match.captured(2).toInt();
        QString errorMessage = match.captured(3);

        Core::EditorManager::cutForwardNavigationHistory();
        Core::EditorManager::addCurrentPositionToNavigationHistory();

        TextEditor::BaseTextEditor *editor = Q_NULLPTR;

        if(fileName == QStringLiteral("<stdin>"))
        {
            editor = qobject_cast<TextEditor::BaseTextEditor *>(Core::EditorManager::currentEditor());
        }
        else if(!m_portPath.isEmpty())
        {
            editor = qobject_cast<TextEditor::BaseTextEditor *>(Core::EditorManager::openEditor(QDir::cleanPath(QDir::fromNativeSeparators(QString(fileName).prepend(m_portPath)))));
        }

        if(editor)
        {
            Core::EditorManager::addCurrentPositionToNavigationHistory();
            editor->gotoLine(lineNumber);

            QTextCursor cursor = editor->textCursor();

            if(cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor))
            {
                editor->editorWidget()->setBlockSelection(cursor);
            }

            Core::EditorManager::activateEditor(editor);
        }

        QMessageBox *box = new QMessageBox(QMessageBox::Critical, QString(), errorMessage, QMessageBox::Ok, Core::ICore::dialogParent());
        connect(box, &QMessageBox::finished, box, &QMessageBox::deleteLater);
        QTimer::singleShot(0, box, &QMessageBox::exec);

        m_errorFilterString = m_errorFilterString.mid(index + match.capturedLength(0));
    }

    m_errorFilterString = m_errorFilterString.right(ERROR_FILTER_MAX_SIZE);
}

void OpenMVPlugin::saveScript()
{
    if(!m_working)
    {
        int answer = QMessageBox::question(Core::ICore::dialogParent(),
            tr("Save Script"),
            tr("Strip comments and convert spaces to tabs?"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);

        if((answer == QMessageBox::Yes) || (answer == QMessageBox::No))
        {
            QFile file(QDir::cleanPath(QDir::fromNativeSeparators(m_portPath)) + QStringLiteral("main.py"));

            if(file.open(QIODevice::WriteOnly))
            {
                QByteArray contents = Core::EditorManager::currentEditor()->document()->contents();

                if(answer == QMessageBox::Yes)
                {
                    QString bytes = QString::fromLatin1(contents);
                    bytes.remove(QRegularExpression(QStringLiteral("^\\s*?\n"), QRegularExpression::MultilineOption));
                    bytes.remove(QRegularExpression(QStringLiteral("^\\s*#.*?\n"), QRegularExpression::MultilineOption));
                    bytes.remove(QRegularExpression(QStringLiteral("\\s*#.*?$"), QRegularExpression::MultilineOption));
                    bytes.replace(QStringLiteral("    "), QStringLiteral("\t"));
                    contents = bytes.toLatin1();
                }

                if(file.write(contents) != contents.size())
                {
                    QMessageBox::critical(Core::ICore::dialogParent(),
                        tr("Save Script"),
                        tr("Error: %L1!").arg(file.errorString()));
                }
            }
            else
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Save Script"),
                    tr("Error: %L1!").arg(file.errorString()));
            }
        }
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Save Script"),
            tr("Busy... please wait..."));
    }
}

void OpenMVPlugin::saveImage(const QPixmap &data)
{
    QSettings *settings = ExtensionSystem::PluginManager::settings();
    settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

    QString path =
        QFileDialog::getSaveFileName(Core::ICore::dialogParent(), tr("Save Image"),
            settings->value(QStringLiteral(LAST_SAVE_IMAGE_PATH), QDir::homePath()).toString(),
            tr("Image Files (*.bmp *.jpg *.jpeg *.png *.ppm)"));

    if(!path.isEmpty())
    {
        if(data.save(path))
        {
            settings->setValue(QStringLiteral(LAST_SAVE_IMAGE_PATH), path);
        }
        else
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                tr("Save Image"),
                tr("Failed to save the image file for an unknown reason!"));
        }
    }

    settings->endGroup();
}

void OpenMVPlugin::saveTemplate(const QRect &rect)
{
    if(!m_working)
    {
        QString drivePath = QDir::cleanPath(QDir::fromNativeSeparators(m_portPath));

        QSettings *settings = ExtensionSystem::PluginManager::settings();
        settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

        QString path =
            QFileDialog::getSaveFileName(Core::ICore::dialogParent(), tr("Save Template"),
                settings->value(QStringLiteral(LAST_SAVE_TEMPLATE_PATH), drivePath).toString(),
                tr("Image Files (*.bmp *.jpg *.jpeg *.pgm *.ppm)"));

        if(!path.isEmpty())
        {
            path = QDir::cleanPath(QDir::fromNativeSeparators(path));

            if((!path.startsWith(drivePath))
            || (!QDir(QFileInfo(path).path()).exists()))
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Save Template"),
                    tr("Please select a valid path on the OpenMV Cam!"));
            }
            else
            {
                QByteArray sendPath = QString(path).remove(0, drivePath.size()).prepend(QChar::fromLatin1('/')).toLatin1();

                if(sendPath.size() <= DESCRIPTOR_SAVE_PATH_MAX_LEN)
                {
                    m_iodevice->templateSave(rect.x(), rect.y(), rect.width(), rect.height(), sendPath);
                    settings->setValue(QStringLiteral(LAST_SAVE_TEMPLATE_PATH), path);
                }
                else
                {
                    QMessageBox::critical(Core::ICore::dialogParent(),
                        tr("Save Template"),
                        tr("\"%L1\" is longer than a max length of %L2 characters!").arg(QString::fromLatin1(sendPath)).arg(DESCRIPTOR_SAVE_PATH_MAX_LEN));
                }
            }
        }

        settings->endGroup();
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Save Template"),
            tr("Busy... please wait..."));
    }
}

void OpenMVPlugin::saveDescriptor(const QRect &rect)
{
    if(!m_working)
    {
        QString drivePath = QDir::cleanPath(QDir::fromNativeSeparators(m_portPath));

        QSettings *settings = ExtensionSystem::PluginManager::settings();
        settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

        QString path =
            QFileDialog::getSaveFileName(Core::ICore::dialogParent(), tr("Save Descriptor"),
                settings->value(QStringLiteral(LAST_SAVE_DESCRIPTOR_PATH), drivePath).toString(),
                tr("Keypoints Files (*.lbp *.orb)"));

        if(!path.isEmpty())
        {
            path = QDir::cleanPath(QDir::fromNativeSeparators(path));

            if((!path.startsWith(drivePath))
            || (!QDir(QFileInfo(path).path()).exists()))
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Save Descriptor"),
                    tr("Please select a valid path on the OpenMV Cam!"));
            }
            else
            {
                QByteArray sendPath = QString(path).remove(0, drivePath.size()).prepend(QChar::fromLatin1('/')).toLatin1();

                if(sendPath.size() <= DESCRIPTOR_SAVE_PATH_MAX_LEN)
                {
                    m_iodevice->descriptorSave(rect.x(), rect.y(), rect.width(), rect.height(), sendPath);
                    settings->setValue(QStringLiteral(LAST_SAVE_DESCRIPTOR_PATH), path);
                }
                else
                {
                    QMessageBox::critical(Core::ICore::dialogParent(),
                        tr("Save Descriptor"),
                        tr("\"%L1\" is longer than a max length of %L2 characters!").arg(QString::fromLatin1(sendPath)).arg(DESCRIPTOR_SAVE_PATH_MAX_LEN));
                }
            }
        }

        settings->endGroup();
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Save Descriptor"),
            tr("Busy... please wait..."));
    }
}

void OpenMVPlugin::updateCam()
{
    if(!m_working)
    {
        QFile file(Core::ICore::userResourcePath() + QStringLiteral("/firmware/firmware.txt"));

        if(file.open(QIODevice::ReadOnly))
        {
            QByteArray data = file.readAll();

            if((file.error() == QFile::NoError) && (!data.isEmpty()))
            {
                file.close();

                QRegularExpressionMatch match = QRegularExpression(QStringLiteral("(\\d+)\\.(\\d+)\\.(\\d+)")).match(QString::fromLatin1(data));

                if((m_major < match.captured(1).toInt())
                || ((m_major == match.captured(1).toInt()) && (m_minor < match.captured(2).toInt()))
                || ((m_major == match.captured(1).toInt()) && (m_minor == match.captured(2).toInt()) && (m_patch < match.captured(3).toInt())))
                {
                    if(QMessageBox::warning(Core::ICore::dialogParent(),
                        tr("Firmware Update"),
                        tr("Update your OpenMV Cam's firmware to the latest version?"),
                        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok)
                    == QMessageBox::Ok)
                    {
                        int answer = QMessageBox::question(Core::ICore::dialogParent(),
                            tr("Firmware Update"),
                            tr("Erase the internal file system?"),
                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);

                        if((answer == QMessageBox::Yes) || (answer == QMessageBox::No))
                        {
                            disconnectClicked();

                            if(pluginSpec()->state() != ExtensionSystem::PluginSpec::Stopped)
                            {
                                connectClicked(true, QString(), answer);
                            }
                        }
                    }
                }
                else
                {
                    QMessageBox::information(Core::ICore::dialogParent(),
                        tr("Firmware Update"),
                        tr("Your OpenMV Cam's firmware is up to date."));

                    if(QMessageBox::question(Core::ICore::dialogParent(),
                        tr("Firmware Update"),
                        tr("Need to reset your OpenMV Cam's firmware to the release version?"),
                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes)
                    == QMessageBox::Yes)
                    {
                        int answer = QMessageBox::question(Core::ICore::dialogParent(),
                            tr("Firmware Update"),
                            tr("Erase the internal file system?"),
                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);

                        if((answer == QMessageBox::Yes) || (answer == QMessageBox::No))
                        {
                            disconnectClicked();

                            if(pluginSpec()->state() != ExtensionSystem::PluginSpec::Stopped)
                            {
                                connectClicked(true, QString(), answer);
                            }
                        }
                    }
                }
            }
            else if(file.error() != QFile::NoError)
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Firmware Update"),
                    tr("Error: %L1!").arg(file.errorString()));
            }
            else
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Firmware Update"),
                    tr("Cannot open firmware.txt!"));
            }
        }
        else
        {
            QMessageBox::critical(Core::ICore::dialogParent(),
                tr("Firmware Update"),
                tr("Error: %L1!").arg(file.errorString()));
        }
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Firmware Update"),
            tr("Busy... please wait..."));
    }
}

void OpenMVPlugin::setPortPath(bool silent)
{
    if(!m_working)
    {
        QStringList drives;

        foreach(const QStorageInfo &info, QStorageInfo::mountedVolumes())
        {
            if(info.isValid()
            && info.isReady()
            && (!info.isRoot())
            && (!info.isReadOnly())
            && (QString::fromLatin1(info.fileSystemType()).contains(QStringLiteral("fat"), Qt::CaseInsensitive) || QString::fromLatin1(info.fileSystemType()).contains(QStringLiteral("msdos"), Qt::CaseInsensitive))
            && ((!Utils::HostOsInfo::isMacHost()) || info.rootPath().startsWith(QStringLiteral("/volumes/"), Qt::CaseInsensitive))
            && ((!Utils::HostOsInfo::isLinuxHost()) || info.rootPath().startsWith(QStringLiteral("/media/"), Qt::CaseInsensitive) || info.rootPath().startsWith(QStringLiteral("/mnt/"), Qt::CaseInsensitive) || info.rootPath().startsWith(QStringLiteral("/run/"), Qt::CaseInsensitive)))
            {
                drives.append(info.rootPath());
            }
        }

        QSettings *settings = ExtensionSystem::PluginManager::settings();
        settings->beginGroup(QStringLiteral(SERIAL_PORT_SETTINGS_GROUP));

        if(drives.isEmpty())
        {
            if(!silent)
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Select Drive"),
                    tr("No valid drives were found to associate with your OpenMV Cam!"));
            }

            m_portPath = QString();
        }
        else if(drives.size() == 1)
        {
            if(m_portPath == drives.first())
            {
                QMessageBox::information(Core::ICore::dialogParent(),
                    tr("Select Drive"),
                    tr("\"%L1\" is the only drive available so it must be your OpenMV Cam's drive.").arg(drives.first()));
            }
            else
            {
                m_portPath = drives.first();
                settings->setValue(m_portName, m_portPath);
            }
        }
        else
        {
            int index = drives.indexOf(settings->value(m_portName).toString());

            bool ok = silent;
            QString temp = silent ? drives.first() : QInputDialog::getItem(Core::ICore::dialogParent(),
                tr("Select Drive"), tr("Please associate a drive with your OpenMV Cam"),
                drives, (index != -1) ? index : 0, false, &ok,
                Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                (Utils::HostOsInfo::isMacHost() ? Qt::WindowType() : Qt::WindowCloseButtonHint));

            if(ok)
            {
                m_portPath = temp;
                settings->setValue(m_portName, m_portPath);
            }
        }

        settings->endGroup();

        m_pathButton->setText((!m_portPath.isEmpty()) ? tr("Drive: %L1").arg(m_portPath) : tr("Drive:"));

        Core::IEditor *editor = Core::EditorManager::currentEditor();
        m_saveCommand->action()->setEnabled((!m_portPath.isEmpty()) && (editor ? (editor->document() ? (!editor->document()->contents().isEmpty()) : false) : false));

        m_frameBuffer->enableSaveTemplate(!m_portPath.isEmpty());
        m_frameBuffer->enableSaveDescriptor(!m_portPath.isEmpty());
    }
    else
    {
        QMessageBox::critical(Core::ICore::dialogParent(),
            tr("Select Drive"),
            tr("Busy... please wait..."));
    }
}

QMap<QString, QAction *> OpenMVPlugin::aboutToShowExamplesRecursive(const QString &path, QMenu *parent)
{
    QMap<QString, QAction *> actions;
    QDirIterator it(path, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);

    while(it.hasNext())
    {
        QString filePath = it.next();

        if(it.fileInfo().isDir())
        {
            QMenu *menu = new QMenu(it.fileName(), parent);
            QMap<QString, QAction *> menuActions = aboutToShowExamplesRecursive(filePath, menu);
            menu->addActions(menuActions.values());
            menu->setDisabled(menuActions.values().isEmpty());
            actions.insertMulti(it.fileName(), menu->menuAction());
        }
        else
        {
            QAction *action = new QAction(it.fileName(), parent);
            connect(action, &QAction::triggered, this, [this, filePath]
            {
                QFile file(filePath);

                if(file.open(QIODevice::ReadOnly))
                {
                    QByteArray data = file.readAll();

                    if((file.error() == QFile::NoError) && (!data.isEmpty()))
                    {
                        Core::EditorManager::cutForwardNavigationHistory();
                        Core::EditorManager::addCurrentPositionToNavigationHistory();

                        QString titlePattern = QFileInfo(filePath).baseName().simplified() + QStringLiteral("_$.") + QFileInfo(filePath).completeSuffix();
                        TextEditor::BaseTextEditor *editor = qobject_cast<TextEditor::BaseTextEditor *>(Core::EditorManager::openEditorWithContents(Core::Constants::K_DEFAULT_TEXT_EDITOR_ID, &titlePattern, data));

                        if(editor)
                        {
                            editor->editorWidget()->configureGenericHighlighter();
                            Core::EditorManager::activateEditor(editor);
                        }
                        else
                        {
                            QMessageBox::critical(Core::ICore::dialogParent(),
                                tr("Open Example"),
                                tr("Cannot open the example file \"%L1\"!").arg(filePath));
                        }
                    }
                    else if(file.error() != QFile::NoError)
                    {
                        QMessageBox::critical(Core::ICore::dialogParent(),
                            tr("Open Example"),
                            tr("Error: %L1!").arg(file.errorString()));
                    }
                    else
                    {
                        QMessageBox::critical(Core::ICore::dialogParent(),
                            tr("Open Example"),
                            tr("Cannot open the example file \"%L1\"!").arg(filePath));
                    }
                }
                else
                {
                    QMessageBox::critical(Core::ICore::dialogParent(),
                        tr("Open Example"),
                        tr("Error: %L1!").arg(file.errorString()));
                }
            });

            actions.insertMulti(it.fileName(), action);
        }
    }

    return actions;
}

void OpenMVPlugin::openThresholdEditor()
{

}

void OpenMVPlugin::openKeypointsEditor()
{
    QMessageBox box(QMessageBox::Question, tr("Keypoints Editor"), tr("What would you like to do?"), QMessageBox::Cancel, Core::ICore::dialogParent(),
        Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
        (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));
    QPushButton *button0 = box.addButton(tr("Edit File"), QMessageBox::AcceptRole);
    QPushButton *button1 = box.addButton(tr("Merge Files"), QMessageBox::AcceptRole);
    box.setDefaultButton(button0);
    box.setEscapeButton(QMessageBox::Cancel);
    box.exec();

    QString drivePath = QDir::cleanPath(QDir::fromNativeSeparators(m_portPath));

    QSettings *settings = ExtensionSystem::PluginManager::settings();
    settings->beginGroup(QStringLiteral(SETTINGS_GROUP));

    if(box.clickedButton() == button0)
    {
        QString path =
            QFileDialog::getOpenFileName(Core::ICore::dialogParent(), tr("Edit Keypoints"),
                settings->value(QStringLiteral(LAST_EDIT_KEYPOINTS_PATH), drivePath.isEmpty() ? QDir::homePath() : drivePath).toString(),
                tr("Keypoints Files (*.lbp *.orb)"));

        if(!path.isEmpty())
        {
            QScopedPointer<Keypoints> ks(Keypoints::newKeypoints(path));

            if(ks)
            {
                QString name = QFileInfo(path).completeBaseName();

                QStringList list = QDir(QFileInfo(path).path()).entryList(QStringList()
                    << (name + QStringLiteral(".bmp"))
                    << (name + QStringLiteral(".jpg"))
                    << (name + QStringLiteral(".jpeg"))
                    << (name + QStringLiteral(".ppm"))
                    << (name + QStringLiteral(".pgm"))
                    << (name + QStringLiteral(".pbm")),
                    QDir::Files,
                    QDir::Name);

                if(!list.isEmpty())
                {
                    KeypointsEditor editor(ks.data(), QPixmap(QFileInfo(path).path() + QDir::separator() + list.first()), Core::ICore::dialogParent(),
                        Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                        (Utils::HostOsInfo::isMacHost() ? Qt::WindowType(0) : Qt::WindowCloseButtonHint));

                    if(editor.exec() == QDialog::Accepted)
                    {
                        if(ks->saveKeypoints(path))
                        {
                            settings->setValue(QStringLiteral(LAST_EDIT_KEYPOINTS_PATH), path);
                        }
                        else
                        {
                            QMessageBox::critical(Core::ICore::dialogParent(),
                                tr("Save Edited Keypoints"),
                                tr("Failed to save the edited keypoints for an unknown reason!"));
                        }
                    }
                }
                else
                {
                    QMessageBox::critical(Core::ICore::dialogParent(),
                        tr("Edit Keypoints"),
                        tr("Failed to find the keypoints image file!"));
                }
            }
            else
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Edit Keypoints"),
                    tr("Failed to load the keypoints file for an unknown reason!"));
            }
        }
    }
    else if(box.clickedButton() == button1)
    {
        QStringList paths =
            QFileDialog::getOpenFileNames(Core::ICore::dialogParent(), tr("Merge Keypoints"),
                QFileInfo(settings->value(QStringLiteral(LAST_MERGE_KEYPOINTS_PATH), drivePath.isEmpty() ? QDir::homePath() : drivePath).toString()).path(),
                tr("Keypoints Files (*.lbp *.orb)"));

        if(!paths.isEmpty())
        {
            QScopedPointer<Keypoints> ks(Keypoints::newKeypoints(paths.takeFirst()));

            if(ks)
            {
                foreach(const QString &path, paths)
                {
                    ks->mergeKeypoints(path);
                }

                QString path =
                    QFileDialog::getSaveFileName(Core::ICore::dialogParent(), tr("Save Merged Keypoints"),
                        settings->value(QStringLiteral(LAST_MERGE_KEYPOINTS_PATH), drivePath).toString(),
                        tr("Keypoints Files (*.lbp *.orb)"));

                if(!path.isEmpty())
                {
                    if(ks->saveKeypoints(path))
                    {
                        settings->setValue(QStringLiteral(LAST_MERGE_KEYPOINTS_PATH), path);
                    }
                    else
                    {
                        QMessageBox::critical(Core::ICore::dialogParent(),
                            tr("Save Merged Keypoints"),
                            tr("Failed to save the merged keypoints for an unknown reason!"));
                    }
                }
            }
            else
            {
                QMessageBox::critical(Core::ICore::dialogParent(),
                    tr("Merge Keypoints"),
                    tr("Failed to load the first keypoints file for an unknown reason!"));
            }
        }
    }

    settings->endGroup();
}

} // namespace Internal
} // namespace OpenMV
