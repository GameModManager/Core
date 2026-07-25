#include <QApplication>
#include <QCommandLineParser>
#include <QDir>

#include "ui/main_window/main_window.h"
#include "engine/log/logger.h"
#include "engine/log/crash_handler.h"

static std::string data_dir() {
    return (QDir::homePath() + "/.local/share/GameModManager").toStdString();
}

static std::string log_path() {
    return data_dir() + "/gamemodmanager.log";
}

static std::string crash_dir() {
    return data_dir() + "/crashes";
}

int main(int argc, char *argv[])
{
    // Initialize crash handler before anything else
    engine::CrashHandler::install(crash_dir());

    QApplication app(argc, argv);
    app.setApplicationName("GameModManager");
    app.setApplicationVersion("0.1.0");
    app.setStyle("fusion");

    // Parse CLI flags
    QCommandLineParser parser;
    parser.setApplicationDescription("GameModManager - Cross-platform game mod manager");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption instanceOpt("instance", "Load specific instance", "name");
    parser.addOption(instanceOpt);

    QCommandLineOption launchOpt("launch", "Launch game directly (headless mode)");
    parser.addOption(launchOpt);

    parser.process(app);

    // Initialize logger
    engine::Logger::instance().set_log_file(log_path());
    engine::Logger::instance().info("GameModManager v" + std::string(VERSION) + " started");

    // Handle CLI flags
    bool headless = parser.isSet(launchOpt);
    QString instance_name;
    if (parser.isSet(instanceOpt)) {
        instance_name = parser.value(instanceOpt);
        engine::Logger::instance().info("Instance: " + instance_name.toStdString());
    }

    if (headless) {
        engine::Logger::instance().info("Headless launch mode");
        // TODO: implement headless launch
        engine::Logger::instance().error("Headless mode not yet implemented");
        return 1;
    }

    ui::MainWindow window;
    if (!instance_name.isEmpty()) {
        window.setWindowTitle("GameModManager - " + instance_name);
    }
    window.show();

    int rc = app.exec();

    engine::Logger::instance().info("GameModManager shutting down");
    engine::CrashHandler::uninstall();
    return rc;
}
