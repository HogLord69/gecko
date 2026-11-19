#include "ModToolsRunner.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPushButton>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QList>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QTimer>
#include <QWidget>
#include <spdlog/spdlog.h>

#include "ui/QtDialogs.h"

#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace geck {

namespace {

    QString toQString(const std::filesystem::path& path) {
        return QString::fromStdU16String(path.u16string());
    }

#ifdef _WIN32
    /// One argument quoted for CreateProcess the way CommandLineToArgvW reads it back: wrapped in
    /// quotes when it has a space, tab or quote; a run of backslashes doubled only before a quote.
    QString quoteArgument(const QString& argument) {
        const bool plain = !argument.isEmpty()
            && std::none_of(argument.begin(), argument.end(),
                [](QChar c) { return c == u' ' || c == u'\t' || c == u'"'; });
        if (plain) {
            return argument;
        }
        QString quoted = QStringLiteral("\"");
        int backslashes = 0;
        for (const QChar c : argument) {
            if (c == u'\\') {
                ++backslashes;
                continue;
            }
            if (c == u'"') {
                quoted += QString(backslashes * 2 + 1, u'\\');
            } else {
                quoted += QString(backslashes, u'\\');
            }
            quoted += c;
            backslashes = 0;
        }
        quoted += QString(backslashes * 2, u'\\');
        quoted += u'"';
        return quoted;
    }
#endif

    QString lastLines(const QByteArray& text, int count) {
        QStringList lines = QString::fromUtf8(text).split(u'\n', Qt::SkipEmptyParts);
        if (lines.size() > count) {
            lines = lines.mid(lines.size() - count);
        }
        return lines.join(u'\n').trimmed();
    }

} // namespace

ModToolsRunner::ModToolsRunner(std::filesystem::path root, QString python, QObject* parent)
    : QObject(parent)
    , _root(std::move(root))
    , _python(python.trimmed().isEmpty() ? QStringLiteral("python") : python.trimmed()) {
}

std::filesystem::path ModToolsRunner::scriptFor(const std::filesystem::path& root) {
    return root / "tools" / "gecko_assets.py";
}

bool ModToolsRunner::isModToolsRoot(const std::filesystem::path& root) {
    std::error_code ec;
    return !root.empty() && std::filesystem::is_regular_file(scriptFor(root), ec);
}

std::filesystem::path ModToolsRunner::detectRootAboveApplication() {
    std::filesystem::path dir(QCoreApplication::applicationDirPath().toStdU16String());
    // The release zip puts the map workflow kit beside gecko.exe (gecko/workflow): its tools are
    // the mod tools of anyone who uses the kit. Only gecko's own folder is checked for one, so a
    // build inside a mod's project still finds that project's tools further up.
    if (isModToolsRoot(dir / "workflow")) {
        return (dir / "workflow").lexically_normal();
    }
    for (int level = 0; level < 8 && !dir.empty(); ++level) {
        if (isModToolsRoot(dir)) {
            return dir.lexically_normal();
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return {};
}

std::filesystem::path ModToolsRunner::resolveRoot(const std::filesystem::path& configured) {
    if (isModToolsRoot(configured)) {
        return configured;
    }
    return detectRootAboveApplication();
}

QStringList ModToolsRunner::scriptArguments(const QStringList& args) const {
    return QStringList{ toQString(scriptFor(_root)) } + args;
}

namespace {
    std::function<QString()>& gameFolderHint() {
        static std::function<QString()> hint;
        return hint;
    }
} // namespace

void ModToolsRunner::setGameFolderHint(std::function<QString()> hint) {
    gameFolderHint() = std::move(hint);
}

bool ModToolsRunner::tryToFix(const ModToolResult& result) {
    if (result.ok) {
        return false;
    }
    const QStringList missing = missingPackages(result);
    if (!missing.isEmpty()) {
        return offerToInstall(missing, result.json.value("python_path").toString());
    }
    if (result.json.value("needs_base").toBool()) {
        return offerToMakeWorkingCopy(result.json);
    }
    return false;
}

QProcess* ModToolsRunner::run(const QStringList& args, std::function<void(const ModToolResult&)> done) {
    // Up to two fixes in a row (the packages, then the working copy), the command run again after each.
    auto attempt = std::make_shared<std::function<QProcess*(int)>>();
    QPointer<ModToolsRunner> self(this);
    *attempt = [self, args, done = std::move(done), attempt](int fixesLeft) -> QProcess* {
        return self->runOnce(args, [self, done, attempt, fixesLeft](const ModToolResult& result) {
            if (self && fixesLeft > 0 && self->tryToFix(result)) {
                (*attempt)(fixesLeft - 1);
                return;
            }
            done(result);
            *attempt = nullptr; // the chain holds itself until it is answered
        });
    };
    return (*attempt)(2);
}

QProcess* ModToolsRunner::runOnce(const QStringList& args, std::function<void(const ModToolResult&)> done) {
    auto* process = new QProcess(this);
    process->setProgram(_python);
    process->setArguments(scriptArguments(args));
    process->setWorkingDirectory(toQString(_root));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process->setProcessEnvironment(environment);

    spdlog::debug("Mod tools: {} {}", _python.toStdString(), args.join(u' ').toStdString());

    // Shared so whichever signal arrives first answers, and only once.
    auto answer = std::make_shared<std::function<void(const ModToolResult&)>>(std::move(done));
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [process, answer](int exitCode, QProcess::ExitStatus) {
            const ModToolResult result
                = parseOutput(exitCode, process->readAllStandardOutput(), process->readAllStandardError());
            process->deleteLater();
            if (*answer) {
                std::exchange(*answer, nullptr)(result);
            }
        });
    connect(process, &QProcess::errorOccurred, this, [this, process, answer](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return; // a crash or kill still ends in finished()
        }
        ModToolResult result;
        result.error = QString("Couldn't start \"%1\". Check the Python setting in File > Preferences > Mod Tools.")
                           .arg(_python);
        process->deleteLater();
        if (*answer) {
            std::exchange(*answer, nullptr)(result);
        }
    });
    process->start();
    return process;
}

ModToolResult ModToolsRunner::runAndWait(const QStringList& args, int timeoutMs) {
    ModToolResult result = runOnceAndWait(args, timeoutMs);
    for (int fixesLeft = 2; fixesLeft > 0 && tryToFix(result); --fixesLeft) {
        result = runOnceAndWait(args, timeoutMs);
    }
    return result;
}

bool ModToolsRunner::offerToMakeWorkingCopy(const QJsonObject& answer) {
    static bool declined = false;
    static bool busy = false;
    const QString setup = answer.value("setup").toString();
    const QString base = answer.value("base").toString();
    if (declined || busy || setup.isEmpty() || !QFileInfo::exists(setup)
        || QGuiApplication::platformName() == QLatin1String("offscreen")) {
        return false; // without the workflow's setup.py the answer's own message says what to do
    }
    const auto holdsGame = [](const QString& folder) {
        return !folder.isEmpty() && QFileInfo::exists(QDir(folder).filePath("master.dat"));
    };
    QString folder = answer.value("fallout2").toString();
    if (!holdsGame(folder) && gameFolderHint()) {
        folder = gameFolderHint()();
    }
    if (!holdsGame(folder)) {
        folder.clear();
    }
    QWidget* parent = QApplication::activeWindow();

    for (;;) {
        QMessageBox ask(parent);
        ask.setIcon(QMessageBox::Question);
        ask.setWindowTitle("Working Copy of Fallout 2");
        ask.setText("The map tools need their own working copy of Fallout 2, made once from your Fallout 2 folder.");
        ask.setInformativeText(folder.isEmpty()
                ? QString("gecko didn't find your Fallout 2 folder: choose it (the one with master.dat in it).")
                : QString("From: %1\nInto: %2\n\nAbout 1.2 GB and a few minutes. Your Fallout 2 folder is only read. It "
                          "should have the Restoration Project Unofficial installed (mods\\rpu.dat in it).")
                      .arg(QDir::toNativeSeparators(folder), QDir::toNativeSeparators(base)));
        QPushButton* make = folder.isEmpty() ? nullptr : ask.addButton("Make It", QMessageBox::AcceptRole);
        QPushButton* choose = ask.addButton(folder.isEmpty() ? "Choose Folder..." : "Another Folder...", QMessageBox::ActionRole);
        ask.addButton(QMessageBox::Cancel);
        ask.setDefaultButton(make ? make : choose);
        ask.exec();
        if (ask.clickedButton() == choose) {
            const QString picked = QtDialogs::selectDirectory(parent, "Your Fallout 2 Folder");
            if (holdsGame(picked)) {
                folder = picked;
            } else if (!picked.isEmpty()) {
                QtDialogs::showWarning(parent, "Working Copy of Fallout 2",
                    "There's no master.dat in that folder. Choose the folder Fallout 2 is installed in.");
            }
            continue;
        }
        if (make == nullptr || ask.clickedButton() != make) {
            declined = true;
            return false;
        }
        break;
    }

    busy = true;
    QProgressDialog progress("Making the working copy of Fallout 2...", QString(), 0, 0, parent);
    progress.setWindowTitle("Working Copy of Fallout 2");
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);
    progress.setMinimumWidth(520);
    progress.show();

    QProcess process;
    process.setProgram(_python);
    process.setArguments({ setup, "--yes", "--build-base", "--fallout2", QDir::toNativeSeparators(folder) });
    process.setWorkingDirectory(toQString(_root));
    process.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    QByteArray output;
    connect(&process, &QProcess::readyRead, &progress, [&process, &output, &progress]() {
        output += process.readAll();
        const QString last = lastLines(output, 1);
        if (!last.isEmpty()) {
            progress.setLabelText("Making the working copy of Fallout 2...\n" + last.left(90));
        }
    });
    QEventLoop loop;
    connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);
    connect(&process, &QProcess::errorOccurred, &loop, [&loop](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            loop.quit();
        }
    });
    QTimer::singleShot(40 * 60 * 1000, &loop, &QEventLoop::quit);
    process.start();
    loop.exec();
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(3000);
    }
    output += process.readAll();
    progress.close();
    busy = false;

    const bool made = QFileInfo::exists(QDir(base).filePath("master.dat"));
    spdlog::info("Mod tools: working copy from {} -> {}", folder.toStdString(), made ? "made" : "failed");
    if (!made) {
        QString detail = lastLines(output, 10);
        if (detail.isEmpty()) {
            detail = process.errorString();
        }
        QtDialogs::showWarning(parent, "Working Copy of Fallout 2",
            QString("The working copy wasn't made:\n\n%1\n\nTo make it by hand, open a Command Prompt in %2 and run:\n"
                    "python setup.py --build-base")
                .arg(detail, QDir::toNativeSeparators(toQString(_root))));
    }
    return made;
}

QStringList ModToolsRunner::missingPackages(const ModToolResult& result) {
    QStringList packages;
    for (const auto& value : result.json.value("missing").toArray()) {
        packages << value.toString();
    }
    if (packages.isEmpty() && result.json.isEmpty()) {
        // No answer at all: Python stopped before the tools could check, on a bare import.
        static const QRegularExpression noModule(QStringLiteral(R"(No module named '([A-Za-z_][A-Za-z0-9_]*))"));
        const QString module = noModule.match(result.error).captured(1);
        static const QHash<QString, QString> pipNames{ { "PIL", "pillow" }, { "numpy", "numpy" }, { "scipy", "scipy" },
            { "cv2", "opencv-python-headless" } };
        if (pipNames.contains(module)) {
            packages << pipNames.value(module);
        }
    }
    packages.removeAll(QString());
    return packages;
}

bool ModToolsRunner::offerToInstall(const QStringList& packages, const QString& pythonPath) {
    static bool declined = false; // asked once a session: no again would only nag
    static bool busy = false;     // a second answer arriving while pip runs waits for the first
    if (declined || busy || packages.isEmpty() || QGuiApplication::platformName() == QLatin1String("offscreen")) {
        return false;
    }
    static const QHash<QString, QString> shown{ { "pillow", "Pillow" }, { "numpy", "NumPy" }, { "scipy", "SciPy" },
        { "opencv-python-headless", "OpenCV" } };
    QStringList names;
    for (const QString& package : packages) {
        names << shown.value(package, package);
    }
    const QString python = QDir::toNativeSeparators(pythonPath.isEmpty() ? _python : pythonPath);
    QWidget* parent = QApplication::activeWindow();

    QMessageBox ask(parent);
    ask.setIcon(QMessageBox::Question);
    ask.setWindowTitle("Python Packages");
    ask.setText(QString("The map tools need %1, which the Python gecko runs them with doesn't have.").arg(names.join(", ")));
    ask.setInformativeText(QString("Install them now? gecko runs pip with that Python:\n%1\n\n"
                                   "It needs the internet and takes a minute or two.")
                               .arg(python));
    ask.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    ask.setDefaultButton(QMessageBox::Yes);
    if (ask.exec() != QMessageBox::Yes) {
        declined = true;
        return false;
    }

    busy = true;
    QProgressDialog progress(QString("Installing %1...").arg(names.join(", ")), QString(), 0, 0, parent);
    progress.setWindowTitle("Python Packages");
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);
    progress.show();

    QProcess pip;
    pip.setProgram(_python);
    pip.setArguments(QStringList{ "-m", "pip", "install", "--disable-pip-version-check" } + packages);
    pip.setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    pip.setProcessEnvironment(environment);
    QEventLoop loop;
    connect(&pip, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);
    connect(&pip, &QProcess::errorOccurred, &loop, [&loop](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            loop.quit();
        }
    });
    QTimer::singleShot(15 * 60 * 1000, &loop, &QEventLoop::quit);
    pip.start();
    loop.exec();
    const bool finished = pip.state() == QProcess::NotRunning;
    if (!finished) {
        pip.kill();
        pip.waitForFinished(3000);
    }
    const bool installed = finished && pip.exitStatus() == QProcess::NormalExit && pip.exitCode() == 0;
    const QByteArray output = pip.readAll();
    progress.close();
    busy = false;
    spdlog::info("Mod tools: pip install {} -> {}", packages.join(u' ').toStdString(), installed ? "installed" : "failed");

    if (!installed) {
        QString detail = lastLines(output, 10);
        if (detail.isEmpty()) {
            detail = pip.errorString();
        }
        QtDialogs::showWarning(parent, "Python Packages",
            QString("pip couldn't install them:\n\n%1\n\nTo install them by hand, open a Command Prompt (not Python "
                    "itself) and run:\n\"%2\" -m pip install %3")
                .arg(detail, python, packages.join(u' ')));
        return false;
    }
    return true;
}

ModToolResult ModToolsRunner::runOnceAndWait(const QStringList& args, int timeoutMs) {
    struct Wait {
        ModToolResult result;
        bool finished = false;
        QEventLoop* loop = nullptr;
    };
    auto wait = std::make_shared<Wait>();
    QEventLoop loop;
    wait->loop = &loop;

    QPointer<QProcess> process = runOnce(args, [wait](const ModToolResult& result) {
        wait->result = result;
        wait->finished = true;
        if (wait->loop) {
            wait->loop->quit();
        }
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    if (!wait->finished) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    wait->loop = nullptr;

    if (!wait->finished) {
        if (process) {
            process->kill();
        }
        ModToolResult result;
        result.error = QString("The mod tools took longer than %1 seconds and were stopped.").arg(timeoutMs / 1000);
        return result;
    }
    return wait->result;
}

ModToolResult ModToolsRunner::parseOutput(int exitCode, const QByteArray& standardOutput, const QByteArray& standardError) {
    ModToolResult result;
    const QList<QByteArray> lines = standardOutput.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray line = it->trimmed();
        if (!line.startsWith('{')) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            result.json = document.object();
            result.ok = result.json.value("ok").toBool();
            result.error = result.json.value("error").toString();
            return result;
        }
    }

    // No answer: Python itself failed (a missing package, a syntax error) or was killed.
    QString detail = lastLines(standardError, 12);
    if (detail.isEmpty()) {
        detail = lastLines(standardOutput, 12);
    }
    result.error = QString("The mod tools stopped without an answer (exit code %1).").arg(exitCode);
    if (!detail.isEmpty()) {
        result.error += "\n\n" + detail;
    }
    return result;
}

bool ModToolsRunner::startApply(qint64 geckoPid, const QString& geckoExe, const QString& mapPath, QString* error) const {
    QStringList arguments = scriptArguments(
        { QStringLiteral("apply"), QStringLiteral("--wait-pid"), QString::number(geckoPid), QStringLiteral("--gecko"), geckoExe });
    if (!mapPath.isEmpty()) {
        arguments << QStringLiteral("--map") << mapPath;
    }

#ifdef _WIN32
    // CreateProcess directly: the deploy has to show in a console window of its own that outlives
    // gecko, and QProcess::startDetached gives a GUI parent's child no console at all.
    QString commandLine = quoteArgument(_python);
    for (const QString& argument : arguments) {
        commandLine += u' ' + quoteArgument(argument);
    }
    std::wstring command = commandLine.toStdWString();
    const std::wstring directory = _root.wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr, directory.c_str(), &startup, &info)) {
        if (error) {
            *error = QString("Couldn't start \"%1\" (Windows error %2). Check the Python setting in "
                             "File > Preferences > Mod Tools.")
                         .arg(_python)
                         .arg(GetLastError());
        }
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
#else
    if (!QProcess::startDetached(_python, arguments, toQString(_root))) {
        if (error) {
            *error = QString("Couldn't start \"%1\". Check the Python setting in Preferences > Mod Tools.").arg(_python);
        }
        return false;
    }
    return true;
#endif
}

} // namespace geck
