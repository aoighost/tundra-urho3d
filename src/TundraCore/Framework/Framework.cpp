// For conditions of distribution and use, see copyright notice in LICENSE

#include "StableHeaders.h"
#include "Framework.h"
#include "JSON.h"

#include "PluginAPI.h"
#include "FrameAPI.h"
#include "ConfigAPI.h"

#include "Scene/SceneAPI.h"
#include "Console/ConsoleAPI.h"
#include "Debug/DebugAPI.h"
#include "UI/UiAPI.h"
#include "Input/InputAPI.h"
#include "Asset/AssetAPI.h"
#include "Asset/AssetCache.h"
#include "Asset/LocalAssetProvider.h"
#include "Asset/LocalAssetStorage.h"

#include "TundraVersionInfo.h"
#include "LoggingFunctions.h"
#include "IModule.h"

#include <Urho3D/Core/Context.h>
#include <Urho3D/Engine/Engine.h>
#include <Urho3D/IO/FileSystem.h>
#include <Urho3D/IO/File.h>
#include <Urho3D/IO/Log.h>
#include <Urho3D/Resource/XMLFile.h>
#include <Urho3D/Core/ProcessUtils.h>
#include <Urho3D/Input/Input.h>
#include <Urho3D/Graphics/Graphics.h>
#include <Urho3D/Core/Profiler.h>

using namespace Urho3D;

namespace Tundra
{

static Framework* instance = 0;
static int argc;
static char** argv;
static String organizationName(TUNDRA_ORGANIZATION_NAME);
static String applicationName(TUNDRA_APPLICATION_NAME);
static String versionString(TUNDRA_VERSION_STRING);
#ifdef ANDROID
static String packageName(TUNDRA_PACKAGE_NAME);
#endif

int run(int argc_, char** argv_)
{
    argc = argc_;
    argv = argv_;

    Context ctx;
    Framework fw(&ctx);

    fw.Initialize();
    fw.Go();
    fw.Uninitialize();

    return 0;
}

void set_run_args(int argc_, char** argv_)
{
    argc = argc_;
    argv = argv_;
}

Framework::Framework(Context* ctx) :
    Object(ctx),
    exitSignal(false),
    headless(false),
    renderer(0)
{
    instance = this;

    // Create the Urho3D engine, which creates various other subsystems, but does not initialize them yet
    engine = new Urho3D::Engine(GetContext());
    // Timestamps clutter the log. Disable for now
    GetSubsystem<Log>()->SetTimeStamp(false);

    ProcessStartupOptions();
    // In headless mode, no main UI/rendering window is initialized.
    headless = HasCommandLineParameter("--headless");

    console = new ConsoleAPI(this);
    frame = new FrameAPI(this);
    plugin = new PluginAPI(this);
    config = new ConfigAPI(this);
    debug = new DebugAPI(this);
    scene = new SceneAPI(this);
    asset = new AssetAPI(this, headless);
    input = new InputAPI(this);
    ui = new UiAPI(this);

    // Prepare main cache directory
    String cacheDir = UserDataDirectory() + "cache";
    if (!GetSubsystem<Urho3D::FileSystem>()->DirExists(cacheDir))
        GetSubsystem<Urho3D::FileSystem>()->CreateDir(cacheDir);

    // Prepare asset cache if enabled.
    String assetCacheDir = cacheDir + "/assets";
    if (CommandLineParameters("--assetCacheDir").Size() > 0)
        assetCacheDir = ParseWildCardFilename(CommandLineParameters("--assetCacheDir").Back());
    if (CommandLineParameters("--assetCacheDir").Size() > 1)
        LogWarning("Multiple --assetCacheDir parameters specified! Using \"" + CommandLineParameters("--assetCacheDir").Back() + "\" as the asset cache directory.");
    if (!HasCommandLineParameter("--noAssetCache"))
        asset->OpenAssetCache(assetCacheDir);

    // Open console window if necessary
    if (headless)
        OpenConsoleWindow();
}

Framework::~Framework()
{
    scene.Reset();
    frame.Reset();
    plugin.Reset();
    config.Reset();
    asset.Reset();
    debug.Reset();
    ui.Reset();

    instance = 0;
}

void Framework::Initialize()
{
    // Urho engine initialization parameters
    VariantMap engineInitMap;

    ApplyStartupOptions(engineInitMap);
    LoadConfig(engineInitMap);

    // Initialization prints
    LogInfo("Installation  " + InstallationDirectory());
    LogInfo("Working       " + CurrentWorkingDirectory());
    LogInfo("Data          " + UserDataDirectory());
    LogInfo("Config        " + config->ConfigFolder());
    LogInfo("Asset cache   " + (asset->Cache() ? asset->Cache()->CacheDirectory() : "Disabled"));

    PrintStartupOptions();

    // Load plugins
    plugin->LoadPluginsFromCommandLine();

    // Initialize the Urho3D engine
    engineInitMap["ResourcePaths"] = GetSubsystem<FileSystem>()->GetProgramDir() + "Data";
    engineInitMap["AutoloadPaths"] = "";
    engineInitMap["Headless"] = headless;
    engineInitMap["WindowResizable"] = true;
    engineInitMap["LogName"] = "Tundra.log";

    LogInfo("");
    engine->Initialize(engineInitMap);
    // Show mouse cursor for more pleasant experience
    input->SetMouseCursorVisible(true);

    // Initialize core APIs
    console->Initialize();

    console->RegisterCommand("plugins", "Prints all currently loaded plugins.", plugin.Get(), &PluginAPI::ListPlugins);
    console->RegisterCommand("exit", "Shuts down gracefully.", this, &Framework::Exit);

    // Initialize plugins now
    LogInfo("");
    LogInfo("Initializing");
    for(uint i = 0; i < modules.Size(); ++i)
    {
        LogInfo("  " + modules[i]->Name());
        modules[i]->Initialize();
    }

    // Set storages from command line options
    SetupAssetStorages();

    // Show profiler immediately if requested
    if (HasCommandLineParameter("--showProfiler"))
        debug->ToggleDebugHudVisibility();
}

void Framework::SetupAssetStorages()
{
    // Add the "Ogre Media" asset storage which contains built-in sky and terrain assets
    /// \todo Scenes should be sanitated to not refer to it
    String systemAssetDir = InstallationDirectory() + "Data/Assets";
    asset->AssetProvider<LocalAssetProvider>()->AddStorageDirectory(systemAssetDir, "Ogre Media", true, false);

    // Add storages from --file first then --storage. First one passed will be set as default (if non have default=true;)
    /// @todo Should the first --storage take priority over --file. Can be used to override the base url for the --file
    StringVector storageSources = CommandLineParameters("--file") + CommandLineParameters("--storage");
    for (uint si=0; si<storageSources.Size(); ++si)
    {
        AssetStoragePtr storage = asset->DeserializeAssetStorageFromString(storageSources[si].Trimmed(), false);
        if (si == 0) // We can't ask if default storage is already set, first storage is returned if not set
            asset->SetDefaultAssetStorage(storage);
    }

    // Set default storage by name if specified
    if (HasCommandLineParameter("--defaultstorage"))
    {
        StringVector defaultStorages = CommandLineParameters("--defaultstorage");
        if (defaultStorages.Size() == 1)
        {
            AssetStoragePtr defaultStorage = asset->AssetStorageByName(defaultStorages[0]);
            if (!defaultStorage)
                LogError("Cannot set storage \"" + defaultStorages[0] + "\" as the default storage, since it doesn't exist!");
            else
                asset->SetDefaultAssetStorage(defaultStorage);
        }
        else
            LogError("Parameter --defaultstorage may be specified exactly once, and must contain a single value!");
    }
}

void Framework::Go()
{
    // Run mainloop
    if (!exitSignal)
    {
        while (!engine->IsExiting())
            ProcessOneFrame();
    }
}

bool Framework::Pump()
{
    if (exitSignal || engine->IsExiting())
        return false;
    ProcessOneFrame();
    return true;
}

void Framework::Uninitialize()
{
    SaveConfig();

    LogDebug("");
    LogDebug("Uninitializing");
    for(uint i = 0; i < modules.Size(); ++i)
    {
        LogDebug("  " + modules[i]->Name());
        modules[i]->Uninitialize();
    }

    // Delete scenes, assets and factories before unloading modules
    scene->Reset();
    asset->Reset();

    LogDebug("Unloading");
    for(uint i = 0; i < modules.Size(); ++i)
    {
        LogDebug("  " + modules[i]->Name());
        modules[i]->Unload();
    }

    // Delete all modules.
    modules.Clear();

    // Actually unload all DLL plugins from memory.
    plugin->UnloadPlugins();
}

void Framework::SaveConfig()
{
    if (!config)
        return;

    /* Add TundraCore related config value saves here.
       This function is called on exit. Urho Engine is
       valid at this point but some subsystems might have
       already been partly destructed eg. Graphics window
       has been closed. */
}

void Framework::LoadConfig(VariantMap &engineInitMap)
{
    if (!config)
        return;

    IntVector2 windowPosition = config->Read(ConfigAPI::FILE_FRAMEWORK, ConfigAPI::SECTION_GRAPHICS, "window position", IntVector2(Urho3D::M_MAX_INT,Urho3D::M_MAX_INT)).GetIntVector2();
    IntVector2 windowSize = config->Read(ConfigAPI::FILE_FRAMEWORK, ConfigAPI::SECTION_GRAPHICS, "window size", IntVector2(1024,768)).GetIntVector2();

    // If position is at 0,0 (from a full screen mode) do not apply it
    if (windowPosition.x_ != Urho3D::M_MAX_INT && windowPosition.y_ != Urho3D::M_MAX_INT && windowPosition.x_ > 0 && windowPosition.y_ > 0)
    {
        engineInitMap["WindowPositionX"] = windowPosition.x_;
        engineInitMap["WindowPositionY"] = windowPosition.y_;
    }
    engineInitMap["WindowWidth"] = windowSize.x_;
    engineInitMap["WindowHeight"] = windowSize.y_;
    engineInitMap["FullScreen"] = config->Read(ConfigAPI::FILE_FRAMEWORK, ConfigAPI::SECTION_GRAPHICS, "window fullscreen", false).GetBool();
}

void Framework::Exit()
{
    exitSignal = true;
    ExitRequested.Emit();
}

void Framework::ForceExit()
{
    exitSignal = true;
    engine->Exit(); // Can not be canceled
}

void Framework::CancelExit()
{
    exitSignal = false;
}

void Framework::ProcessOneFrame()
{
    float dt = engine->GetNextTimeStep();

    Time* time = GetSubsystem<Time>();
    time->BeginFrame(dt);

    for(unsigned i = 0; i < modules.Size(); ++i)
        modules[i]->Update(dt);

    asset->Update(dt);
    input->Update(dt);
    frame->Update(dt);

    /// \todo remove Android hack: exit by pressing back button, which is mapped to ESC
#ifdef ANDROID
    if (GetSubsystem<Urho3D::Input>()->GetKeyPress(KEY_ESC))
        Exit();
#endif

    // Perform Urho engine update/render/measure next timestep
    engine->Update();
    engine->Render();
    engine->ApplyFrameLimit();

    if (HasCommandLineParameter("--logProfilerEachFrame"))
    {
        // Android does not tolerate long log lines (cuts output past certain point), therefore split and log each row separately
        StringVector lines = GetSubsystem<Urho3D::Profiler>()->PrintData(false, false).Split('\n');
        for (uint i = 0; i < lines.Size(); ++i)
            LogInfo(lines[i]);
        GetSubsystem<Urho3D::Profiler>()->BeginInterval();
    }

    time->EndFrame();

    if (exitSignal)
        engine->Exit();
}

void Framework::RegisterModule(IModule *module)
{
    modules.Push(SharedPtr<IModule>(module));
    module->Load();
}

IModule *Framework::ModuleByName(const String &name) const
{
    for(unsigned i = 0; i < modules.Size(); ++i)
        if (modules[i]->Name() == name)
            return modules[i].Get();
    return nullptr;
}

FrameAPI* Framework::Frame() const
{
    return frame;
}

ConfigAPI* Framework::Config() const
{
    return config;
}

PluginAPI* Framework::Plugin() const
{
    return plugin;
}

SceneAPI* Framework::Scene() const
{
    return scene;
}

ConsoleAPI* Framework::Console() const
{
    return console;
}

AssetAPI* Framework::Asset() const
{
    return asset;
}

DebugAPI *Framework::Debug() const
{
    return debug;
}

InputAPI *Framework::Input() const
{
    return input;
}

UiAPI *Framework::Ui() const
{
    return ui;
}

Engine* Framework::Engine() const
{
    return engine;
}

void Framework::RegisterRenderer(IRenderer *renderer_)
{
    renderer = renderer_;
}

IRenderer *Framework::Renderer() const
{
    return renderer;
}

Framework* Framework::Instance()
{
    return instance;
}

void Framework::SetCurrentWorkingDirectory(const String& newCwd)
{
    GetSubsystem<FileSystem>()->SetCurrentDir(newCwd);
}

String Framework::CurrentWorkingDirectory() const
{
    return GetSubsystem<FileSystem>()->GetCurrentDir();
}

String Framework::InstallationDirectory() const
{
    return GetSubsystem<FileSystem>()->GetProgramDir();
}

#if defined(ANDROID)

#include <SDL_System.h>

/* This functionality is coming to the next SDL release
   https://github.com/spurious/SDL-mirror/blob/master/src/filesystem/android/SDL_sysfilesystem.c
   Remove this code then and let urho FileSystem take care of it once release is out and Urho version is upgraded.
   org and app input params are ignored as resolves to android app data directory that already contains app
   specifiers via package name. */
char *Tundra_Android_SDL_GetPrefPath(const char* /*org*/, const char* /*app*/)
{
    const char *path = SDL_AndroidGetInternalStoragePath();
    if (path) {
        size_t pathlen = SDL_strlen(path)+2;
        char *fullpath = (char *)SDL_malloc(pathlen);
        if (!fullpath) {
            SDL_OutOfMemory();
            return NULL;
        }
        SDL_snprintf(fullpath, pathlen, "%s/", path);
        return fullpath;
    }
    return NULL;
}
#endif

String Framework::UserDataDirectory() const
{
#if !defined(ANDROID)
    return GetInternalPath(GetSubsystem<FileSystem>()->GetAppPreferencesDir(OrganizationName(), ApplicationName()));
#else
    // See above Tundra_SDL_GetPrefPath
    String dir;
    char *prefPath = Tundra_Android_SDL_GetPrefPath(OrganizationName().CString(), ApplicationName().CString());
    if (prefPath)
    {
        dir = GetInternalPath(String(prefPath));
        SDL_free(prefPath);
    }
    else
        LogWarning("Could not get application preferences directory: " + String(SDL_GetError()));
    return dir;
#endif
}

String Framework::UserDocumentsDirectory() const
{
    return GetSubsystem<FileSystem>()->GetUserDocumentsDir();
}

String Framework::ParseWildCardFilename(const String& input) const
{
    // Parse all the special symbols from the log filename.
    String filename = input.Trimmed().Replaced("$(CWD)/", CurrentWorkingDirectory());
    filename = filename.Replaced("$(INSTDIR)/", InstallationDirectory());
    filename = filename.Replaced("$(USERDATA)/", UserDataDirectory());
    filename = filename.Replaced("$(USERDOCS)/", UserDocumentsDirectory());
    return filename;
}

String Framework::LookupRelativePath(String path) const
{
    FileSystem* fs = GetSubsystem<FileSystem>();

    // If a relative path was specified, lookup from cwd first, then from application installation directory.
    if (!IsAbsolutePath(path))
    {
        // On Android always refer to the installation directory (inside APK) for relative paths
#ifdef ANDROID
        return InstallationDirectory() + path;
#endif
        String cwdPath = CurrentWorkingDirectory() + path;
        if (fs->FileExists(cwdPath))
            return cwdPath;
        else
            return InstallationDirectory() + path;
    }
    else
        return path;
}

void Framework::AddCommandLineParameter(const String &command, const String &parameter)
{
    String commandLowercase = command.ToLower();
    startupOptions[commandLowercase].first_ = command;
    startupOptions[commandLowercase].second_.Push(parameter);
}

bool Framework::HasCommandLineParameter(const String &value) const
{
    String valueLowercase = value.ToLower();
    if (value == "--config")
        return !configFiles.Empty();

    return startupOptions.Find(valueLowercase) != startupOptions.End();
}

Vector<String> Framework::CommandLineParameters(const String &key) const
{
    String keyLowercase = key.ToLower();
    if (key == "--config")
        return ConfigFiles();
    
    OptionsMap::ConstIterator i = startupOptions.Find(keyLowercase);
    if (i != startupOptions.End())
        return i->second_.second_;
    else
        return Vector<String>();
}

void Framework::ProcessStartupOptions()
{
    for(int i = 1; i < argc; ++i)
    {
        String option(argv[i]);
        String peekOption = (i+1 < argc ? String(argv[i+1]) : "");
        if (option.StartsWith("--") && !peekOption.Empty())
        {
#ifdef WIN32
            // --key "value
            if (peekOption.StartsWith("\""))
            {
                // --key "value"
                if (peekOption.EndsWith("\""))
                {
                    // Remove quotes and append to the return list.
                    peekOption = peekOption.Substring(1, peekOption.Length() - 1);
                }
                // --key "val u e"
                else
                {
                    for(int pi=i+2; pi+1 < argc; ++pi)
                    {
                        // If a new -- key is found before an end quote we have a error.
                        // Report and don't add anything to the return list as the param is malformed.
                        String param = argv[pi];
                        if (param.StartsWith("--"))
                        {
                            LogError("Could not find an end quote for '" + option + "' parameter: " + peekOption);
                            i = pi - 1; // Step one back so the main for loop will inspect this element next.
                            break;
                        }
                        
                        peekOption += " " + param;
                        if (param.EndsWith("\""))
                        {
                            if (peekOption.StartsWith("\""))
                                peekOption = peekOption.Substring(1);
                            if (peekOption.EndsWith("\""))
                                peekOption.Resize(peekOption.Length() - 1);

                            // Set the main for loops index so it will skip the 
                            // parts that included in this quoted param.
                            i = pi; 
                            break;
                        }
                    }
                }
            }
#endif
        }
        else if (option.StartsWith("--") && peekOption.Empty())
            AddCommandLineParameter(option);
        else
        {
            LogWarning("Orphaned startup option parameter value specified: " + String(argv[i]));
            continue;
        }

        if (option.Trimmed().Empty())
            continue;

        // --config
        if (option.Compare("--config", false) == 0)
        {
            LoadStartupOptionsFromFile(peekOption);
            ++i;
            continue;
        }

        // --key value
        if (!peekOption.StartsWith("--"))
        {
            AddCommandLineParameter(option, peekOption);
            ++i;
        }
        // --key
        else
            AddCommandLineParameter(option);
    }

    if (!HasCommandLineParameter("--config"))
        LoadStartupOptionsFromFile("tundra.json");
}

void Framework::ApplyStartupOptions(VariantMap &engineInitMap)
{
    String windowTitle = "Tundra";
    StringVector titleParams = CommandLineParameters("--windowTitle");
    if (!titleParams.Empty())
        windowTitle = titleParams.Front();
    engineInitMap["WindowTitle"] = windowTitle;

    String windowIcon = "Textures/icon-32x32.png";
    StringVector iconParams = CommandLineParameters("--windowIcon");
    if (!iconParams.Empty())
        windowIcon = iconParams.Front();
    engineInitMap["WindowIcon"] = windowIcon;

    // --loglevel controls both shell/console and file logging
    StringVector logLevelParam  = CommandLineParameters("--loglevel");
    if (logLevelParam.Size() > 1)
        LogWarning("Multiple --loglevel parameters specified! Using " + logLevelParam.Front() + " as the value.");
    if (logLevelParam.Size() > 0)
    {
        String level = logLevelParam.Front();
        if (level.Compare("debug", false) == 0 || level.Compare("verbose", false) == 0)
        {
            engineInitMap["LogLevel"] = Urho3D::LOG_DEBUG;
            console->SetLogLevel(level);
        }
        else if (level.Compare("warn", false) == 0 || level.Compare("warning", false) == 0)
        {
            engineInitMap["LogLevel"] = Urho3D::LOG_WARNING;
            console->SetLogLevel(level);
        }
        else if (level.Compare("error", false) == 0)
        {
            engineInitMap["LogLevel"] = Urho3D::LOG_ERROR;
            console->SetLogLevel(level);
        }
        else if (level.Compare("none", false) == 0 || level.Compare("disabled", false) == 0)
        {
            engineInitMap["LogLevel"] = Urho3D::LOG_NONE;
            console->SetLogLevel(level);
        }
        else
            LogWarning("Erroneous --loglevel: " + logLevelParam.Front() + ". Ignoring.");

        // Apply the log level now, as there will be logging on Tundra side before engine->Initialize()
        Log* log = engine->GetSubsystem<Log>();
        if (log)
            log->SetLevel(Engine::GetParameter(engineInitMap, "LogLevel", Urho3D::LOG_INFO).GetInt());
    }
    // --quiet silences < LOG_ERROR from shell/console but stil writes as per --loglevel to file log
    if (HasCommandLineParameter("--quiet"))
    {
        engineInitMap["LogQuiet"] = true;
        Log* log = engine->GetSubsystem<Log>();
        if (log)
            log->SetQuiet(true);
    }
    if (HasCommandLineParameter("--touchEmulation"))
        engineInitMap["TouchEmulation"] = true;

    // Prepare ConfigAPI data folder
    StringVector configDirs = CommandLineParameters("--configDir");
    String configDir = "$(USERDATA)/configuration"; // The default configuration goes to "C:\Users\username\AppData\Roaming\Tundra\configuration"
    if (configDirs.Size() >= 1)
        configDir = configDirs.Back();
    if (configDirs.Size() > 1)
        LogWarning("Multiple --configDir parameters specified! Using \"" + configDir + "\" as the configuration directory.");
    config->PrepareDataFolder(configDir);

    // Set target FPS limits, if specified.
    ConfigData targetFpsConfigData(ConfigAPI::FILE_FRAMEWORK, ConfigAPI::SECTION_RENDERING);
    if (config->HasKey(targetFpsConfigData, "fps target limit"))
    {
        int targetFps = config->Read(targetFpsConfigData, "fps target limit").GetInt();
        if (targetFps >= 0)
            engine->SetMaxFps(targetFps);
        else
            LogWarning("Invalid target FPS value " + String(targetFps) + " read from config. Ignoring.");
    }

    StringVector fpsLimitParam = CommandLineParameters("--fpsLimit");
    if (fpsLimitParam.Size() > 1)
        LogWarning("Multiple --fpslimit parameters specified! Using " + fpsLimitParam.Front() + " as the value.");
    if (fpsLimitParam.Size() > 0)
    {
        int targetFpsLimit = ToInt(fpsLimitParam.Front());
        if (targetFpsLimit >= 0)
            engine->SetMaxFps(targetFpsLimit);
        else
            LogWarning("Erroneous FPS limit given with --fpsLimit: " + fpsLimitParam.Front() + ". Ignoring.");
    }

    StringVector fpsLimitInactiveParam = CommandLineParameters("--fpsLimitWhenInactive");
    if (fpsLimitInactiveParam.Size() > 1)
        LogWarning("Multiple --fpslimit parameters specified! Using " + fpsLimitInactiveParam.Front() + " as the value.");
    if (fpsLimitInactiveParam.Size() > 0)
    {
        int targetFpsLimit = ToInt(fpsLimitParam.Front());
        if (targetFpsLimit >= 0)
            engine->SetMaxInactiveFps(targetFpsLimit);
        else
            LogWarning("Erroneous FPS limit given with --fpsLimit: " + fpsLimitInactiveParam.Front() + ". Ignoring.");
    }

    if (CommandLineParameters("--antialias").Size() > 0) // "Full screen antialiasing factor"
        engineInitMap["MultiSample"] = Urho3D::ToInt(CommandLineParameters("--antialias").Front());
    else if (Config()->HasKey(ConfigAPI::FILE_FRAMEWORK, ConfigAPI::SECTION_RENDERING, "antialias"))
        engineInitMap["MultiSample"] = Config()->Read(ConfigAPI::FILE_FRAMEWORK, ConfigAPI::SECTION_RENDERING, "antialias").GetInt();
}

void Framework::PrintStartupOptions()
{
    LogInfo("");
    LogInfo("Startup options");
    for (OptionsMap::ConstIterator i = startupOptions.Begin(); i != startupOptions.End(); ++i)
    {
        String option = i->second_.first_;
        LogInfo("  " + option);
        for (unsigned j = 0; j < i->second_.second_.Size(); ++j)
        {
            if (!i->second_.second_[j].Empty())
                LogInfo("    '" + i->second_.second_[j] + "'");
        }
    }
}

bool Framework::LoadStartupOptionsFromFile(const String &configurationFile)
{
    String suffix = GetExtension(configurationFile);
    bool read = false;
    if (suffix == ".xml")
        read = LoadStartupOptionsFromXML(configurationFile);
    else if (suffix == ".json")
        read = LoadStartupOptionsFromJSON(configurationFile);
    else
        LogError("Invalid config file format. Only .xml and .json are supported: " + configurationFile);
    if (read)
        configFiles.Push(configurationFile);
    return read;
}

bool Framework::LoadStartupOptionsFromXML(String configurationFile)
{
    configurationFile = LookupRelativePath(configurationFile);

    Urho3D::XMLFile doc(GetContext());
    File file(GetContext(), configurationFile, FILE_READ);
    if (!doc.Load(file))
    {
        LogError("Failed to open config file \"" + configurationFile + "\"!");
        return false;
    }

    XMLElement root = doc.GetRoot();

    XMLElement e = root.GetChild("option");
    while(e)
    {
        if (e.HasAttribute("name"))
        {
            /// \todo Support build exclusion

            /// If we have another config XML specified with --config inside this config XML, load those settings also
            if (e.GetAttribute("name").Compare("--config", false) == 0)
            {
                if (!e.GetAttribute("value").Empty())
                    LoadStartupOptionsFromFile(e.GetAttribute("value"));
                e = e.GetNext("option");
                continue;
            }

            AddCommandLineParameter(e.GetAttribute("name"), e.GetAttribute("value"));
        }
        e = e.GetNext("option");
    }
    return true;
}

bool Framework::LoadStartupOptionsFromJSON(String configurationFile)
{
    configurationFile = LookupRelativePath(configurationFile);
    
    File file(GetContext(), configurationFile, FILE_READ);
    if (!file.IsOpen())
    {
        LogError("Failed to open config file \"" + configurationFile + "\"!");
        return false;
    }
    JSONValue root;
    if (!root.FromString(file.ReadString()))
    {
        LogError("Failed to parse config file \"" + configurationFile + "\"!");
        return false;
    }

    if (root.IsArray())
        LoadStartupOptionArray(root);
    else if (root.IsObject())
        LoadStartupOptionMap(root);
    else if (root.IsString())
        AddCommandLineParameter(root.GetString());
    else
        LogError("JSON config file " + configurationFile + " was not an object, array or string");
    
    return true;
}

void Framework::LoadStartupOptionArray(const JSONValue& value)
{
    const JSONArray& arr = value.GetArray();
    for (JSONArray::ConstIterator i = arr.Begin(); i != arr.End(); ++i)
    {
        const JSONValue& innerValue = *i;
        if (innerValue.IsArray())
            LoadStartupOptionArray(innerValue);
        else if (innerValue.IsObject())
            LoadStartupOptionMap(innerValue);
        else if (innerValue.IsString())
            AddCommandLineParameter(innerValue.GetString());
    }
}

void Framework::LoadStartupOptionMap(const JSONValue& value)
{
    const JSONObject& obj = value.GetObject();
    for (JSONObject::ConstIterator i = obj.Begin(); i != obj.End(); ++i)
    {
        /// \todo Support build and platform exclusion

        String option = i->first_;
        if (i->second_.IsString())
        {
            if (option.Compare("--config", false) == 0)
                LoadStartupOptionsFromFile(i->second_.GetString());
            else
                AddCommandLineParameter(option, i->second_.GetString());
        }
        else if (i->second_.IsArray())
        {
            const JSONArray& innerArr = i->second_.GetArray();
            for (JSONArray::ConstIterator j = innerArr.Begin(); j != innerArr.End(); ++j)
            {
                if (j->IsString())
                {
                    if (option.Compare("--config", false) == 0)
                        LoadStartupOptionsFromFile(j->GetString());
                    else
                        AddCommandLineParameter(option, j->GetString());
                }
            }
        }
    }
}

const String& Framework::OrganizationName()
{
    return organizationName;
}

const String& Framework::ApplicationName()
{
    return applicationName;
}

#ifdef ANDROID
const String& Framework::PackageName()
{
    return packageName;
}

#endif

const String& Framework::VersionString()
{
    return versionString;
}

}
