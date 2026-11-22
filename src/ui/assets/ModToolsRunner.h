#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <vector>

class QProcess;
class QProcessEnvironment;

namespace geck {

/// One answer from the mod's tools/gecko_assets.py: its last line of output is a JSON object,
/// {"ok": true, ...} or {"ok": false, "error": "..."}.
struct ModToolResult {
    bool ok = false;
    QJsonObject json; ///< the whole answer, when there was one
    QString error;    ///< the tool's own message, or why it could not run
};

/**
 * Runs the mod's tools/gecko_assets.py for the Tools menu: scenery from a picture, mirrored and
 * resized copies, names and examine text, and Apply New Art. The mod's rules (names, ids, the
 * registry, the deploy chain) all live on the Python side; this starts it and reads the answer.
 *
 * The mod folder and the Python to run it come from Preferences > Mod Tools; with no Python named
 * there, the one that comes with gecko (python/ beside gecko.exe in the release zip).
 */
class ModToolsRunner : public QObject {
    Q_OBJECT

public:
    ModToolsRunner(std::filesystem::path root, QString python, QObject* parent = nullptr);

    /// The Python that comes with gecko: python/python.exe in @p applicationDir (the release zip carries
    /// one with the packages the tools need). Empty when there is none.
    static QString bundledPython(const QString& applicationDir = QCoreApplication::applicationDirPath());
    /// The Python the tools run with for a Preferences setting of @p configured. An empty setting or a bare
    /// python / python.exe (whatever PATH finds first, which can be the Microsoft Store's: Windows hides the
    /// new files that one writes under AppData from every other program, gecko included) means the one that
    /// comes with gecko when there is one, else python from PATH. Any other setting is used as given.
    static QString effectivePython(
        const QString& configured, const QString& applicationDir = QCoreApplication::applicationDirPath());

    /// root/tools/gecko_assets.py
    static std::filesystem::path scriptFor(const std::filesystem::path& root);
    /// Whether @p root holds the script, i.e. the Tools menu can work.
    static bool isModToolsRoot(const std::filesystem::path& root);
    /// The map workflow folder beside gecko.exe (the release zip's layout), else the nearest folder
    /// above gecko.exe that holds the script, or empty: a mod that builds gecko inside its own project
    /// needs no setting there.
    static std::filesystem::path detectRootAboveApplication();
    /// The mod folder the Tools menu uses: @p configured (Preferences > Mod Tools) when it holds
    /// the script, else the one found above gecko.exe, else empty.
    static std::filesystem::path resolveRoot(const std::filesystem::path& configured);

    /// The map workflow's working copy of the game: config.json's "base" (a relative one is under @p root)
    /// or @p root/base. Empty when @p root is empty; the folder need not exist yet.
    static std::filesystem::path workingCopyFolder(const std::filesystem::path& root);
    /// The folder the map workflow's deploy (Apply New Art) writes the mod into: <working copy>/mods/foc.
    /// Empty when @p root is empty or nothing has been deployed yet.
    static std::filesystem::path deployedModFolder(const std::filesystem::path& root);
    /// The data paths that read the game the way the working copy has it, lowest priority first: its
    /// master.dat and critter.dat, the patch archives the game loads over them (patch000.dat, ...), the
    /// mods in its mods/mods_order.txt (the Restoration Project first), its loose files, then the deployed
    /// mod. Only those that exist. What `python foc.py edit` sets: kept in step with foc_data_paths in
    /// workflow/tools/build_gecko_browsers.py. Empty when there is no working copy (no master.dat in it).
    static std::vector<std::filesystem::path> workingCopyDataPaths(const std::filesystem::path& root);
    /// @p dataPaths with the deployed mod mounted last, on top of the game, so what Apply New Art put in
    /// (new pieces, roof tiles in spare tile lines) draws whichever game folder Preferences names.
    /// Unchanged when nothing is deployed, when a data path already mounts it (a profile that puts the
    /// mod among folders of its own), or when @p browsingAnotherGame: the mod's lists would replace
    /// that game's.
    static std::vector<std::filesystem::path> withDeployedMod(std::vector<std::filesystem::path> dataPaths,
        const std::filesystem::path& root, bool browsingAnotherGame);
    /// What gecko mounts from the tools on top of @p dataPaths: the deployed mod (withDeployedMod), then
    /// the overlay of pieces brought in from other games since that deploy (root/build/gecko_assets/
    /// overlay, which the next deploy removes), so both draw after a restart. Kept in memory, never
    /// saved as data paths; nothing while @p browsingAnotherGame.
    static std::vector<std::filesystem::path> withToolsData(std::vector<std::filesystem::path> dataPaths,
        const std::filesystem::path& root, bool browsingAnotherGame);

    [[nodiscard]] const std::filesystem::path& root() const { return _root; }
    [[nodiscard]] const QString& python() const { return _python; }

    /// Starts a command without blocking; @p done receives the answer on the GUI thread. When the
    /// runner is destroyed first, the process is killed and @p done is never called.
    QProcess* run(const QStringList& args, std::function<void(const ModToolResult&)> done);

    /// Runs a command and waits for it in a local event loop (the window keeps painting; input is
    /// held back). Gives up after @p timeoutMs and kills the process.
    ModToolResult runAndWait(const QStringList& args, int timeoutMs = 180000);

    /// Starts `apply` in a console window of its own, independent of gecko: it waits for
    /// @p geckoPid to exit, deploys the mod, and opens @p geckoExe again (on @p mapPath if given).
    bool startApply(qint64 geckoPid, const QString& geckoExe, const QString& mapPath, QString* error) const;

    /// The answer in a finished command's output: the last line of stdout that is a JSON object.
    static ModToolResult parseOutput(int exitCode, const QByteArray& standardOutput, const QByteArray& standardError);

    /// The Python packages (pip names) an answer says the tools' Python lacks: its "missing" list, or
    /// the package a bare "No module named ..." names when the tools couldn't answer at all.
    static QStringList missingPackages(const ModToolResult& result);

    /// The Fallout 2 folder gecko itself loads (from its data paths), offered when the map workflow
    /// has no working copy yet and its own setup doesn't know one. Set once by the main window.
    static void setGameFolderHint(std::function<QString()> hint);
    /// Called with the mod folder once a command that had to make the working copy first has answered,
    /// so the main window can offer to read the game from it. Set once by the main window.
    static void setWorkingCopyMadeHook(std::function<void(const std::filesystem::path&)> hook);

private:
    QStringList scriptArguments(const QStringList& args) const;
    /// The environment the tools' Python runs in: UTF-8 output, and with the Python that comes with gecko,
    /// no user site-packages (another Python 3.13's pip install --user would shadow its own packages).
    QProcessEnvironment pythonEnvironment() const;
    /// run() / runAndWait() without the offer to install missing packages.
    QProcess* runOnce(const QStringList& args, std::function<void(const ModToolResult&)> done);
    ModToolResult runOnceAndWait(const QStringList& args, int timeoutMs);
    /// Ask to install `packages` with this runner's Python, and do it; true once they are installed.
    /// Asked at most once a session if the answer is no.
    bool offerToInstall(const QStringList& packages, const QString& pythonPath);
    /// Ask to make the map workflow's working copy of Fallout 2 (its setup.py --build-base, from a
    /// Fallout 2 folder) when an answer says it is missing; true once it is there.
    bool offerToMakeWorkingCopy(const QJsonObject& answer);
    /// Offer what fixes a failed answer (packages, then the working copy); true when something was fixed
    /// and the command is worth running again.
    bool tryToFix(const ModToolResult& result);

    std::filesystem::path _root;
    QString _python;
    bool _madeWorkingCopy = false; ///< offerToMakeWorkingCopy made it during the current command
};

} // namespace geck
