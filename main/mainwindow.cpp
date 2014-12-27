#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "recording/samplemover.h"
#include "recording/trackcontroller.h"
#include "recording/trackviewpane.h"
#include "error/simulationwidget.h"
#include "config/database.h"
#include "util/misc.h"
#include "util/boolsignalor.h"
#include "main/configpane.h"
#include "main/quitdialog.h"
#include "main/aboutpane.h"

#ifdef Q_OS_WIN32
#   include "presentation/presentationtab.h"
#endif

#include <QDebug>
#include <QFile>
#include <QCloseEvent>
#include <QThread>
#include <QCommonStyle>
#include <QShortcut>
#include <portaudio.h>

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_config(new Config::Database(this)),
    m_mover(new Recording::SampleMover(m_config->readTotalSamplesRecorded())),
    m_trackController(new Recording::TrackController(m_config, this)),
    m_trackPane(new Recording::TrackViewPane(m_trackController, this)),
    m_configPane(new ConfigPane(m_config, this)),
    m_quitDialog(new QuitDialog(this)),
    m_moverThread(new QThread())
{
    ui->setupUi(this);

#ifdef Q_OS_WIN32
    this->setAttribute(Qt::WA_TranslucentBackground); // WS_LAYERED
    this->setAttribute(Qt::WA_NoSystemBackground, false);
#endif

    // setup the thread first
    m_mover->moveToThread(m_moverThread);
    QObject::connect(m_moverThread, &QThread::finished, m_mover, &QObject::deleteLater); // this is legal and even recommended by the docs for QThread::finished
    m_moverThread->start();

    QObject::connect(ui->recordBtn, &QAbstractButton::toggled, m_mover, &Recording::SampleMover::setRecordingState);
    QObject::connect(ui->trackBtn, &QAbstractButton::clicked, this, &MainWindow::trackBtnClicked);
    QObject::connect(ui->trackNextBtn, &QAbstractButton::clicked, this, &MainWindow::trackBtnClicked);
    QObject::connect(ui->monitorBtn, &QAbstractButton::toggled, m_mover, &Recording::SampleMover::setMonitorEnabled);

    QObject::connect(m_configPane, &ConfigPane::recordDeviceChanged, m_mover, &Recording::SampleMover::setInputDevice);
    QObject::connect(m_configPane, &ConfigPane::monitorDeviceChanged, m_mover, &Recording::SampleMover::setMonitorDevice);
    QObject::connect(m_configPane, &ConfigPane::monitorDeviceChanged, m_trackPane, &Recording::TrackViewPane::setMonitorDevice);
    QObject::connect(m_configPane, &ConfigPane::availableAudioSpaceChanged, this, &MainWindow::availableSpaceUpdate);
    QObject::connect(m_configPane, &ConfigPane::audioDataDirChanged, m_mover, &Recording::SampleMover::setAudioDataDir);

    QObject::connect(m_mover, &Recording::SampleMover::timeUpdate, this, &MainWindow::timeUpdate);
    QObject::connect(m_mover, &Recording::SampleMover::levelMeterUpdate, this, &MainWindow::levelMeterUpdate);
    QObject::connect(m_mover, &Recording::SampleMover::recordingStateChanged, ui->recordBtn, &QAbstractButton::setChecked);
    QObject::connect(m_mover, &Recording::SampleMover::recordingStateChanged, ui->trackBtn, &QAbstractButton::setEnabled);
    QObject::connect(m_mover, &Recording::SampleMover::canMonitorChanged, ui->monitorBtn, &QAbstractButton::setEnabled);
    QObject::connect(m_mover->getDeviceErrorProvider(), &Error::Provider::error, m_configPane, &ConfigPane::displayDeviceError);
    QObject::connect(m_mover->getRecordingErrorProvider(), &Error::Provider::error, ui->recordingErrorDisplay, &Error::Widget::displayError);
    QObject::connect(m_mover, &Recording::SampleMover::recordingStateChanged, m_configPane, &ConfigPane::recordingStateChanged);
    QObject::connect(m_mover, &Recording::SampleMover::recordingStateChanged, m_trackController, &Recording::TrackController::onRecordingStateChanged);
    QObject::connect(m_mover, &Recording::SampleMover::newRecordingFile, m_trackController, &Recording::TrackController::onRecordingFileChanged);
    QObject::connect(m_mover, &Recording::SampleMover::timeUpdate, m_trackController, &Recording::TrackController::onTimeUpdate);
    QObject::connect(m_mover, &Recording::SampleMover::canRecordChanged, ui->recordBtn, &QAbstractButton::setEnabled);

    Util::BoolSignalOr *trackAndNextBtnEnabler = new Util::BoolSignalOr(this);
    QObject::connect(m_mover, &Recording::SampleMover::recordingStateChanged, trackAndNextBtnEnabler, &Util::BoolSignalOr::inputA);
    QObject::connect(trackAndNextBtnEnabler, &Util::BoolSignalOr::output, ui->trackNextBtn, &QAbstractButton::setEnabled);

    QObject::connect(m_trackController, &Recording::TrackController::currentTrackTimeChanged, this, &MainWindow::trackTimeUpdate);

    QObject::connect(m_quitDialog, &QDialog::finished, this, &MainWindow::quitDialogFinished);

    // the designer can't set multiple shortcuts, so we'll do it ourselves
    QShortcut *nextSlideDown = new QShortcut(QKeySequence("Down"), this);
    QObject::connect(nextSlideDown, &QShortcut::activated, ui->nextBtn, &QAbstractButton::click);
    QShortcut *prevSlideUp = new QShortcut(QKeySequence("Up"), this);
    QObject::connect(prevSlideUp, &QShortcut::activated, ui->prevBtn, &QAbstractButton::click);

    ui->levelL->setMinimum(0);
    ui->levelL->setMaximum(std::numeric_limits<int16_t>::max());
    ui->levelR->setMinimum(0);
    ui->levelR->setMaximum(std::numeric_limits<int16_t>::max());

    ui->tabWidget->addTab(m_configPane, tr("Configuration"));
    ui->tabWidget->addTab(m_trackPane, tr("Recording"));

#ifdef Q_OS_WIN32
    m_presentation = new Presentation::PresentationTab(this);

    QObject::connect(m_configPane, &ConfigPane::presentationScreenChanged, m_presentation, &Presentation::PresentationTab::screenUpdated);

    QObject::connect(ui->trackNextBtn, &QAbstractButton::clicked, m_presentation, &Presentation::PresentationTab::nextSlide);
    QObject::connect(ui->nextBtn, &QAbstractButton::clicked, m_presentation, &Presentation::PresentationTab::nextSlide);
    QObject::connect(ui->prevBtn, &QAbstractButton::clicked, m_presentation, &Presentation::PresentationTab::previousSlide);
    QObject::connect(ui->freezeBtn, &QAbstractButton::toggled, m_presentation, &Presentation::PresentationTab::freeze);
    QObject::connect(ui->blankBtn, &QAbstractButton::toggled, m_presentation, &Presentation::PresentationTab::blank);
    QObject::connect(m_presentation, &Presentation::PresentationTab::freezeChanged, ui->freezeBtn, &QAbstractButton::setChecked);
    QObject::connect(m_presentation, &Presentation::PresentationTab::blankChanged, ui->blankBtn, &QAbstractButton::setChecked);
    QObject::connect(m_presentation, &Presentation::PresentationTab::canNextSlideChanged, ui->nextBtn, &QAbstractButton::setEnabled);
    QObject::connect(m_presentation, &Presentation::PresentationTab::canNextSlideChanged, trackAndNextBtnEnabler, &Util::BoolSignalOr::inputB);
    QObject::connect(m_presentation, &Presentation::PresentationTab::canPrevSlideChanged, ui->prevBtn, &QAbstractButton::setEnabled);

    ui->freezeBtn->setEnabled(true);
    ui->blankBtn->setEnabled(true);

    ui->tabWidget->addTab(m_presentation, tr("Presentation"));
#endif

#ifndef QT_NO_DEBUG
    ui->tabWidget->addTab(new Error::SimulationWidget(this), "Debug");
#endif
    ui->tabWidget->addTab(new AboutPane(this), tr("About"));


    // == Main styling ==
    QPalette pal = this->palette();
    pal.setBrush(QPalette::Background, QColor(0x30, 0x30, 0x30));
    pal.setBrush(QPalette::Foreground, QColor(0xFF, 0xFF, 0xFF));
    pal.setBrush(QPalette::Light, QColor(0x40, 0x40, 0x40));
    this->setPalette(pal);
    this->setAutoFillBackground(true);

    // == tab widget styling ==
    // We use a normal QTabWidget, but style the tabs according
    // to our "custom theme" using CSS
    ui->tabWidget->setPalette(QGuiApplication::palette());
    for (int i = 0; i < ui->tabWidget->count(); ++i) {
        QWidget *w = ui->tabWidget->widget(i);
        w->setAutoFillBackground(true);
        w->setBackgroundRole(QPalette::Base);
        w->setPalette(QGuiApplication::palette());
    }
    ui->tabWidget->setStyleSheet(QString(
        "QTabBar::tab {"
        "   background-color: %1;"
        "   color: %2;"
        "   padding: 10px;"
        "   border: 0;"
        "   margin-left: 5px;"
        "}"
        "QTabBar::tab:selected {"
        "   background-color: %3;"
        "   color: %4;"
        "   margin-left: 0;"
        "}"
        "QTabWidget::pane {"
        "  border: 0"
        "}"
    ).arg(this->palette().color(QPalette::Light).name())
     .arg(this->palette().color(QPalette::Foreground).name())
     .arg(ui->tabWidget->palette().color(QPalette::Base).name())
     .arg(ui->tabWidget->palette().color(QPalette::Text).name()));

    // == bottom button styling ==
    // This can be easily done via style sheets, saving us from subclassing QPushButton
    ui->bottomButtonBox->setStyleSheet(QString(
        "QPushButton {"
        "   background-color: %3;"
        "   color: %2;"
        "   border: 1px solid %3;"
        "   outline: 0;"
        "   padding: 2px 10px;"
        "}"
        "QPushButton:!enabled {"
        "   background-color: %1;"
        "}"
        "QPushButton:pressed, QPushButton:checked {"
        "   background-color: %2;"
        "   color: %1;"
        "   border-color: %2;"
        "}"
        "QPushButton:hover {"
        "   border-color: %2;"
        "}"
        "QPushButton:checked:hover, QPushButton:pressed:hover {"
        "   border-color: %3;"
        "   background-color: %2;"
        "}"
    ).replace("%1", this->palette().color(QPalette::Window).name())
     .replace("%2", this->palette().color(QPalette::Foreground).name())
     .replace("%3", this->palette().color(QPalette::Light).name()));

    m_configPane->init();
}

MainWindow::~MainWindow()
{
    m_moverThread->quit();
    m_moverThread->wait();
    delete m_moverThread;
}

void
MainWindow::levelMeterUpdate(int16_t left, int16_t right)
{
    ui->levelL->setValue(qBound((int16_t)0, left, std::numeric_limits<int16_t>::max()));
    ui->levelR->setValue(qBound((int16_t)0, right, std::numeric_limits<int16_t>::max()));
}

void
MainWindow::timeUpdate(uint64_t samples)
{
    ui->totalRecTimeLbl->setText(Util::formatTime(samples));
    ui->diskSpaceLbl->setText(Util::formatTime(m_lastSpaceCheckSpace/4 - (samples - m_lastSpaceCheckTime)));
}

void MainWindow::availableSpaceUpdate(uint64_t space)
{
    m_lastSpaceCheckSpace = space;
    m_lastSpaceCheckTime  = m_mover->getRecordedSampleCount();

    ui->diskSpaceLbl->setText(Util::formatTime(space/4));
}

void MainWindow::trackTimeUpdate(uint64_t samples)
{
    ui->trackTimeLbl->setText(Util::formatTime(samples));
}


void MainWindow::trackBtnClicked()
{
    m_trackController->startNewTrack(m_mover->getRecordedSampleCount());
}

void MainWindow::quitDialogFinished(int result)
{
    if (result == QuitDialog::ABORT)
        return;

    if (result == QuitDialog::DISCARD_DATA) {
        // delete all data files
        auto trackFiles = m_config->readAllFiles();
        for (const auto& file : trackFiles) {
            if (!QFile::remove(file.second))
                qWarning() << "Couldn't remove file: " << file.second;
        }
        // clear the database
        m_config->clearTracksAndFiles();
    }

    // quit
    QCoreApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    ev->ignore();

    if (m_trackController->getTrackCount())
        m_quitDialog->show();
    else
        quitDialogFinished(QuitDialog::DISCARD_DATA);
}
