// For conditions of distribution and use, see copyright notice in LICENSE

#pragma once

#include "TundraCoreApi.h"
#include "FrameworkFwd.h"
#include "Signals.h"

#include <Object.h>

namespace Tundra
{

class JSONValue;

typedef Urho3D::HashMap<Urho3D::String, Urho3D::Pair<Urho3D::String, Urho3D::Vector<Urho3D::String> > > OptionsMap;

/// The system root access object.
class TUNDRACORE_API Framework : public Urho3D::Object 
{
    OBJECT(Framework);

public:
    explicit Framework(Urho3D::Context* ctx);
    ~Framework();

    /// Run the main loop until exit requested.
    void Go();

    /// Runs through a single frame of logic update and rendering.
    void ProcessOneFrame();

    /// Returns module by class T.
    /** @param T class type of the module.
        @return The module, or null if the module doesn't exist. Always remember to check for null pointer. */
    template <class T>
    T *Module() const;

    /// Returns module by name.
    /** @param name Name of the module. */
    IModule *ModuleByName(const Urho3D::String& name) const;

    /// Registers a new module into the Framework.
    /** Framework will take ownership of the module pointer, so it is safe to pass in a raw pointer. */
    void RegisterModule(IModule *module);

    /// Sets the current working directory. Use with caution.
    void SetCurrentWorkingDirectory(const Urho3D::String& newCwd);

    /// Returns the cwd of the current environment.
    /** This directory should not be relied, since it might change due to external code running.
        Always prefer to use InstallationDirectory, UserDataDirectory and UserDocumentsDirectory instead.
        The returned path contains a trailing slash. */
    Urho3D::String CurrentWorkingDirectory() const;

    /// Returns the directory where Tundra was installed to.
    /** This is *always* the directory Tundra.exe resides in.
        E.g. on Windows 7 this is usually of form "C:\Program Files (x86)\Tundra 1.0.5\".
        The returned path contains a trailing slash. */
    Urho3D::String  InstallationDirectory() const;

    /// Returns the directory that is used for Tundra data storage on the current user.
    /** E.g. on Windows 7 this is usually of form "C:\Users\username\AppData\Roaming\Tundra\".
        The returned path contains a trailing slash. */
    Urho3D::String UserDataDirectory() const;

    /// Returns the directory where the documents (for Tundra) of the current user are located in.
    /** E.g. on Windows 7 this is usually of form "C:\Users\username\Documents\Tundra\".
        The returned path contains a trailing slash. */
    Urho3D::String UserDocumentsDirectory() const;

    /// Return organization of the application, e.g. "realXtend".
    static const Urho3D::String& OrganizationName();

    /// Returns name of the application, "Tundra-urho3d" by default.
    static const Urho3D::String& ApplicationName();

#ifdef ANDROID
    /// Returns Android package name.
    static const Urho3D::String& PackageName();
#endif

    /// Returns application version string.
    static const Urho3D::String& VersionString();

    /// Parse a filename for specific wildcard modifiers, and return as parsed
    /** $(CWD) is expanded to the current working directory.
        $(INSTDIR) is expanded to the Tundra installation directory (Application::InstallationDirectory)
        $(USERDATA) is expanded to Application::UserDataDirectory.
        $(USERDOCS) is expanded to Application::UserDocumentsDirectory. */
    Urho3D::String ParseWildCardFilename(const Urho3D::String& input) const;

    /// Returns whether or not the command line arguments contain a specific value.
    /** @param value Key or value with possible prefixes, case-insensitive. */
    bool HasCommandLineParameter(const Urho3D::String &value) const;

    /// Returns list of command line parameter values for a specific @c key, f.ex. "--file".
    /** Value is considered to be the command line argument following the @c key.
        If the argument following @c key is another key-type argument (--something), it's not appended to the return list.
        @param key Key with possible prefixes, case-insensitive */
    Urho3D::Vector<Urho3D::String> CommandLineParameters(const Urho3D::String &key) const;

    /// Returns list of all the config XML filenames specified on command line or within another config XML
    Urho3D::Vector<Urho3D::String> ConfigFiles() const { return configFiles; }

    /// Processes command line options and stores them into a multimap
    void ProcessStartupOptions();

    /// Prints to console all the used startup options.
    void PrintStartupOptions();

    /// Lookup a filename relative to either the installation or current working directory.
    Urho3D::String LookupRelativePath(Urho3D::String path) const;

    /// Request application exit. Exit request signal will be sent and the exit can be canceled by calling CancelExit();
    void Exit();

    /// Forcibly exit application, can not be canceled.
    void ForceExit();

    /// Cancel exit request.
    void CancelExit();

    /// Return whether is headless (no rendering)
    bool IsHeadless() const { return headless; }

    /// Returns core API Plugin object.
    PluginAPI* Plugin() const { return plugin; }

    /// Return the Urho3D Engine object.
    Urho3D::Engine* Engine() const;

    /// Exit request signal.
    Signal0<void> exitRequested;

private:
    /// Adds new command line parameter (option | value pair)
    void AddCommandLineParameter(const Urho3D::String &command, const Urho3D::String &parameter = "");

    /// Directs to XML of JSON parsing function depending on file suffix.
    bool LoadStartupOptionsFromFile(const Urho3D::String &configurationFile);
    
    /// Appends all found startup options from the given file to the startupOptions member.
    bool LoadStartupOptionsFromXML(Urho3D::String configurationFile);
    
    /// Appends all found startup options from the given file to the startupOptions member.
    bool LoadStartupOptionsFromJSON(Urho3D::String configurationFile);

    /// Load a JSON array to startup options.
    void LoadStartupOptionArray(const JSONValue& value);

    /// Load a JSON map to startup options.
    void LoadStartupOptionMap(const JSONValue& value);

    /// Urho3D engine
    Urho3D::SharedPtr<Urho3D::Engine> engine;
    /// Framework owns the memory of all the modules in the system. These are freed when Framework is exiting.
    Urho3D::Vector<Urho3D::SharedPtr<IModule> > modules;
    /// PluginAPI
    Urho3D::SharedPtr<PluginAPI> plugin;
    /// ConfigAPI
    Urho3D::SharedPtr<ConfigAPI> config;
    /// Stores all command line parameters and expanded options specified in the Config XML files, except for the config file(s) themselves.
    OptionsMap startupOptions;
    /// Stores config XML filenames
    Urho3D::Vector<Urho3D::String> configFiles;
    /// Exiting flag. When raised, the Framework will exit on the next main loop iteration
    bool exitSignal;
    /// Headless flag. When headless, no rendering window is created
    bool headless;
};

template <class T>
T *Framework::Module() const
{
    for(unsigned i = 0; i < modules.Size(); ++i)
    {
        T *module = dynamic_cast<T*>(modules[i].Get());
        if (module)
            return module;
    }

    return nullptr;
}

/// Instantiate the Framework and run until exited.
TUNDRACORE_API int run(int argc, char** argv);

}
