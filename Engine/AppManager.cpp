//  Natron
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 *Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 *contact: immarespond at gmail dot com
 *
 */

#include "AppManager.h"

#include <clocale>

#include <QDebug>
#include <QAbstractSocket>
#include <QCoreApplication>

#include "Global/MemoryInfo.h"

#include "Engine/AppInstance.h"
#include "Engine/OfxHost.h"
#include "Engine/Settings.h"
#include "Engine/LibraryBinary.h"
#include "Engine/ProcessHandler.h"
#include "Engine/Node.h"
#include "Engine/Plugin.h"
#include "Engine/ViewerInstance.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Image.h"
#include "Engine/FrameEntry.h"
#include "Engine/Format.h"
#include "Engine/Log.h"

using namespace Natron;

AppManager* AppManager::_instance = 0;

struct AppManagerPrivate {
    
    AppManager::AppType _appType; //< the type of app
    std::map<int,AppInstance*> _appInstances; //< the instances mapped against their ID
    int _availableID; //< the ID for the next instance
    int _topLevelInstanceID; //< the top level app ID
    boost::shared_ptr<Settings> _settings; //< app settings
    std::vector<Format*> _formats; //<a list of the "base" formats available in the application
    std::vector<Natron::Plugin*> _plugins; //< list of the plugins
    boost::scoped_ptr<Natron::OfxHost> ofxHost; //< OpenFX host
    boost::scoped_ptr<KnobFactory> _knobFactory; //< knob maker
    boost::shared_ptr<Natron::Cache<Natron::Image> >  _nodeCache; //< Images cache
    boost::shared_ptr<Natron::Cache<Natron::FrameEntry> > _viewerCache; //< Viewer textures cache
    ProcessInputChannel* _backgroundIPC; //< object used to communicate with the main app
    //if this app is background, see the ProcessInputChannel def
    bool _loaded; //< true when the first instance is completly loaded.
    QString _binaryPath; //< the path to the application's binary

    AppManagerPrivate()
        : _appType(AppManager::APP_BACKGROUND)
        , _appInstances()
        , _availableID(0)
        , _topLevelInstanceID(0)
        , _settings(new Settings(NULL))
        , _formats()
        , _plugins()
        , ofxHost(new Natron::OfxHost())
        , _knobFactory(new KnobFactory())
        , _nodeCache()
        , _viewerCache()
        ,_backgroundIPC(0)
        ,_loaded(false)
        ,_binaryPath()
    {
        
    }
    
    void initProcessInputChannel(const QString& mainProcessServerName);

    
    void loadBuiltinFormats();
    

};


void AppManager::printBackGroundWelcomeMessage(){
    std::cout << "================================================================================" << std::endl;
    std::cout << NATRON_APPLICATION_NAME << "    " << " version: " << NATRON_VERSION_STRING << std::endl;
    std::cout << ">>>Running in background mode (off-screen rendering only).<<<" << std::endl;
    std::cout << "Please note that the background mode is in early stage and accepts only project files "
                 "that would produce a valid output from the graphical version of " NATRON_APPLICATION_NAME << std::endl;
    std::cout << "If the background mode doesn't output any result, please adjust your project via the application interface "
                 "and then re-try using the background mode." << std::endl;
}

void AppManager::printUsage() {
    std::cout << NATRON_APPLICATION_NAME << " usage: " << std::endl;
    std::cout << "./" NATRON_APPLICATION_NAME "    <project file path>" << std::endl;
    std::cout << "[--background] or [-b] enables background mode rendering. No graphical interface will be shown." << std::endl;
    std::cout << "[--writer <Writer node name>] When in background mode, the renderer will only try to render with the node"
                 " name following the --writer argument. If no such node exists in the project file, the process will abort."
                 "Note that if you don't pass the --writer argument, it will try to start rendering with all the writers in the project's file."<< std::endl;

}

bool AppManager::parseCmdLineArgs(int argc,char* argv[],
                                  bool* isBackground,
                                  QString& projectFilename,
                                  QStringList& writers,
                                  QString& mainProcessServerName) {
    
    if (!argv) {
        return false;
    }
    
    *isBackground = false;
    bool expectWriterNameOnNextArg = false;
    bool expectPipeFileNameOnNextArg = false;
    
    QStringList args;
    for(int i = 0; i < argc ;++i){
        args.push_back(QString(argv[i]));
    }
    
    for (int i = 0 ; i < args.size(); ++i) {
        
        if (args.at(i).contains("." NATRON_PROJECT_FILE_EXT)) {
            if(expectWriterNameOnNextArg || expectPipeFileNameOnNextArg) {
                AppManager::printUsage();
                return false;
            }
            projectFilename = args.at(i);
            continue;
        } else if (args.at(i) == "--background" || args.at(i) == "-b") {
            if(expectWriterNameOnNextArg  || expectPipeFileNameOnNextArg){
                AppManager::printUsage();
                return false;
            }
            *isBackground = true;
            continue;
        } else if (args.at(i) == "--writer" || args.at(i) == "-w") {
            if(expectWriterNameOnNextArg  || expectPipeFileNameOnNextArg){
                AppManager::printUsage();
                return false;
            }
            expectWriterNameOnNextArg = true;
            continue;
        } else if (args.at(i) == "--IPCpipe") {
            if (expectWriterNameOnNextArg || expectPipeFileNameOnNextArg) {
                AppManager::printUsage();
                return false;
            }
            expectPipeFileNameOnNextArg = true;
            continue;
        }
        
        if (expectWriterNameOnNextArg) {
            assert(!expectPipeFileNameOnNextArg);
            writers << args.at(i);
            expectWriterNameOnNextArg = false;
        }
        if (expectPipeFileNameOnNextArg) {
            assert(!expectWriterNameOnNextArg);
            mainProcessServerName = args.at(i);
            expectPipeFileNameOnNextArg = false;
        }
    }

    return true;
}

AppManager::AppManager()
    : QObject()
    , _imp(new AppManagerPrivate())
{
    assert(!_instance);
    _instance = this;
}


AppManager::~AppManager(){
    
    AppInstance* mainInstance = getAppInstance(0);
    assert(mainInstance);
    delete mainInstance;
    
    assert(_imp->_appInstances.empty());
    
    _imp->_settings->saveSettings();
    
    for(U32 i = 0; i < _imp->_plugins.size();++i){
        delete _imp->_plugins[i];
    }
    foreach(Format* f,_imp->_formats){
        delete f;
    }
    
    
    if (_imp->_backgroundIPC) {
        delete _imp->_backgroundIPC;
    }
    
    _imp->_nodeCache.reset();
    _imp->_viewerCache.reset();
    _imp->_knobFactory.reset();
    _imp->_settings.reset();
    _imp->ofxHost.reset();
    
    _instance = 0;
}

void AppManager::initializeQApp(int argc,char* argv[]) const {
    new QCoreApplication(argc,argv);
}

bool AppManager::loadInternal(const QString& projectFilename,const QStringList& writers,const QString& mainProcessServerName) {
    assert(!_imp->_loaded);

    _imp->_binaryPath = QCoreApplication::applicationDirPath();
    
    registerEngineMetaTypes();
    registerGuiMetaTypes();

    qApp->setOrganizationName(NATRON_ORGANIZATION_NAME);
    qApp->setOrganizationDomain(NATRON_ORGANIZATION_DOMAIN);
    qApp->setApplicationName(NATRON_APPLICATION_NAME);


    // Natron is not yet internationalized, so it is better for now to use the "C" locale,
    // until it is tested for robustness against locale choice.
    // The locale affects numerics printing and scanning, date and time.
    // Note that with other locales (e.g. "de" or "fr"), the floating-point numbers may have
    // a comma (",") as the decimal separator instead of a point (".").
    // There is also an OpenCOlorIO issue with non-C numeric locales:
    // https://github.com/imageworks/OpenColorIO/issues/297
    //
    // this must be done after initializing the QCoreApplication, see
    // https://qt-project.org/doc/qt-5/qcoreapplication.html#locale-settings
    //std::setlocale(LC_NUMERIC,"C"); // set the locale for LC_NUMERIC only
    std::setlocale(LC_ALL,"C"); // set the locale for everything

    Natron::Log::instance();//< enable logging

    ///basically show a splashScreen
    initGui();

    _imp->_settings->initializeKnobsPublic();


    QObject::connect(_imp->ofxHost.get(), SIGNAL(toolButtonAdded(QStringList,QString,QString,QString,QString)),
                     this, SLOT(addPluginToolButtons(QStringList,QString,QString,QString,QString)));
    size_t maxCacheRAM = _imp->_settings->getRamMaximumPercent() * getSystemTotalRAM();
    U64 maxDiskCache = _imp->_settings->getMaximumDiskCacheSize();
    U64 playbackSize = maxCacheRAM * _imp->_settings->getRamPlaybackMaximumPercent();

    setLoadingStatus("Restoring the image cache...");
    _imp->_nodeCache.reset(new Cache<Image>("NodeCache",0x1, maxCacheRAM - playbackSize,1));
    _imp->_viewerCache.reset(new Cache<FrameEntry>("ViewerCache",0x1,maxDiskCache,(double)playbackSize / (double)maxDiskCache));

    qDebug() << "NodeCache RAM size: " << printAsRAM(_imp->_nodeCache->getMaximumMemorySize());
    qDebug() << "ViewerCache RAM size (playback-cache): " << printAsRAM(_imp->_viewerCache->getMaximumMemorySize());
    qDebug() << "ViewerCache disk size: " << printAsRAM(maxDiskCache);


    
    setLoadingStatus("Restoring user settings...");


    _imp->_settings->restoreSettings();

    ///and save these restored settings in case some couldn't be found
    _imp->_settings->saveSettings();
    
    /*loading all plugins*/
    loadAllPlugins();
    _imp->loadBuiltinFormats();
     

    if (isBackground() && !mainProcessServerName.isEmpty()) {
        _imp->initProcessInputChannel(mainProcessServerName);
        printBackGroundWelcomeMessage();
    }


    if (isBackground()) {
        if (!projectFilename.isEmpty()) {
            _imp->_appType = APP_BACKGROUND_AUTO_RUN;
        } else {
            _imp->_appType = APP_BACKGROUND;
        }
    } else {
        _imp->_appType = APP_GUI;
    }

    AppInstance* mainInstance = newAppInstance(projectFilename,writers);

    hideSplashScreen();

    if (!mainInstance) {
        return false;
    } else {
        return true;
    }
}

bool AppManager::load(int argc, char *argv[]) {
    
    
    ///if the user didn't specify launch arguments (e.g unit testing)
    ///find out the binary path
    bool hadArgs = true;
    if (!argv) {
        QString binaryPath = QDir::currentPath();
        argc = 1;
        argv = new char*[1];
        argv[0] = new char[binaryPath.size() + 1];
        for (int i = 0; i < binaryPath.size(); ++i) {
            argv[0][i] = binaryPath.at(i).unicode();
        }
        argv[0][binaryPath.size()] = '\0';
        hadArgs = false;
    }
    initializeQApp(argc, argv);
    
    assert(argv);
    if (!hadArgs) {
        delete [] argv[0];
        delete [] argv;
    }

    QString projectFilename,mainProcessServerName;
    QStringList writers;
    bool bg;
    AppManager::parseCmdLineArgs(argc, argv,&bg ,projectFilename, writers, mainProcessServerName);

    return loadInternal(projectFilename,writers,mainProcessServerName);

}

bool AppManager::load(const QString& projectFilename,const QStringList& writers,const QString& mainProcessServerName) {
    ///cannot load a backround auto run app without a filename
    if (projectFilename.isEmpty()) {
        return false;
    }
    
    ///the QCoreApplication must have been created so far.
    assert(qApp);
    return loadInternal(projectFilename,writers,mainProcessServerName);

}




AppInstance* AppManager::newAppInstance(const QString& projectName,const QStringList& writers){
    AppInstance* instance = makeNewInstance(_imp->_availableID);
    try {
        instance->load(projectName,writers);
    } catch (const std::exception& e) {
        Natron::errorDialog(NATRON_APPLICATION_NAME, std::string("Cannot create project") + ": " + e.what());
        removeInstance(_imp->_availableID);
        delete instance;
        return NULL;
    } catch (...) {
        Natron::errorDialog(NATRON_APPLICATION_NAME, std::string("Cannot create project"));
        removeInstance(_imp->_availableID);
        delete instance;
        return NULL;
    }
    ++_imp->_availableID;
    
    ///flag that we finished loading the Appmanager even if it was already true
    _imp->_loaded = true;
    return instance;
}

AppInstance* AppManager::getAppInstance(int appID) const{
    std::map<int,AppInstance*>::const_iterator it;
    it = _imp->_appInstances.find(appID);
    if(it != _imp->_appInstances.end()){
        return it->second;
    }else{
        return NULL;
    }
}

const std::map<int,AppInstance*>&  AppManager::getAppInstances() const{
    return _imp->_appInstances;
}

void AppManager::removeInstance(int appID){
    _imp->_appInstances.erase(appID);
}

AppManager::AppType AppManager::getAppType() const {
    return _imp->_appType;
}

void AppManager::clearPlaybackCache(){
    _imp->_viewerCache->clearInMemoryPortion();

}


void AppManager::clearDiskCache(){
    _imp->_viewerCache->clear();
    
}


void AppManager::clearNodeCache(){
    _imp->_nodeCache->clear();
    
}

void AppManager::clearPluginsLoadedCache() {
    _imp->ofxHost->clearPluginsLoadedCache();
}

void AppManager::clearAllCaches() {
    clearDiskCache();
    clearNodeCache();
    
    ///for each app instance clear all its nodes cache
    for (std::map<int,AppInstance*>::iterator it = _imp->_appInstances.begin(); it!= _imp->_appInstances.end(); ++it) {
        it->second->clearOpenFXPluginsCaches();
    }
}

std::vector<LibraryBinary*> AppManager::loadPlugins(const QString &where){
    std::vector<LibraryBinary*> ret;
    QDir d(where);
    if (d.isReadable())
    {
        QStringList filters;
        filters << QString(QString("*.")+QString(NATRON_LIBRARY_EXT));
        d.setNameFilters(filters);
        QStringList fileList = d.entryList();
        for(int i = 0 ; i < fileList.size() ; ++i) {
            QString filename = fileList.at(i);
            if(filename.endsWith(".dll") || filename.endsWith(".dylib") || filename.endsWith(".so")){
                QString className;
                int index = filename.lastIndexOf("." NATRON_LIBRARY_EXT);
                className = filename.left(index);
                std::string binaryPath = NATRON_PLUGINS_PATH + className.toStdString() + "." + NATRON_LIBRARY_EXT;
                LibraryBinary* plugin = new LibraryBinary(binaryPath);
                if(!plugin->isValid()){
                    delete plugin;
                }else{
                    ret.push_back(plugin);
                }
            }else{
                continue;
            }
        }
    }
    return ret;
}

std::vector<Natron::LibraryBinary*> AppManager::loadPluginsAndFindFunctions(const QString& where,
                                                                            const std::vector<std::string>& functions){
    std::vector<LibraryBinary*> ret;
    std::vector<LibraryBinary*> loadedLibraries = loadPlugins(where);
    for (U32 i = 0; i < loadedLibraries.size(); ++i) {
        if (loadedLibraries[i]->loadFunctions(functions)) {
            ret.push_back(loadedLibraries[i]);
        }
    }
    return ret;
}

AppInstance* AppManager::getTopLevelInstance () const{
    std::map<int,AppInstance*>::const_iterator it = _imp->_appInstances.find(_imp->_topLevelInstanceID);
    if(it == _imp->_appInstances.end()){
        return NULL;
    }else{
        return it->second;
    }
}



bool AppManager::isLoaded() const {
    return _imp->_loaded;
}


void AppManagerPrivate::initProcessInputChannel(const QString& mainProcessServerName) {
    _backgroundIPC = new ProcessInputChannel(mainProcessServerName);
}


void AppManager::abortAnyProcessing() {
    for (std::map<int,AppInstance*>::iterator it = _imp->_appInstances.begin(); it!= _imp->_appInstances.end(); ++it) {
        std::vector<Natron::Node*> nodes;
        it->second->getActiveNodes(&nodes);
        for (U32 i = 0; i < nodes.size(); ++i) {
            nodes[i]->quitAnyProcessing();
        }
    }
}

bool AppManager::writeToOutputPipe(const QString& longMessage,const QString& shortMessage) {
    if(!_imp->_backgroundIPC) {
        qDebug() << longMessage;
        return false;
    }
    _imp->_backgroundIPC->writeToOutputChannel(shortMessage);
    return true;
}

void AppManager::registerAppInstance(AppInstance* app){
    _imp->_appInstances.insert(std::make_pair(app->getAppID(),app));
}

void AppManager::setApplicationsCachesMaximumMemoryPercent(double p){
    size_t maxCacheRAM = p * getSystemTotalRAM();
    U64 playbackSize = maxCacheRAM * _imp->_settings->getRamPlaybackMaximumPercent();
    _imp->_nodeCache->setMaximumCacheSize(maxCacheRAM - playbackSize);
    _imp->_nodeCache->setMaximumInMemorySize(1);
    U64 maxDiskCacheSize = _imp->_settings->getMaximumDiskCacheSize();
    _imp->_viewerCache->setMaximumInMemorySize((double)playbackSize / (double)maxDiskCacheSize);
    
    qDebug() << "NodeCache RAM size: " << printAsRAM(_imp->_nodeCache->getMaximumMemorySize());
    qDebug() << "ViewerCache RAM size (playback-cache): " << printAsRAM(_imp->_viewerCache->getMaximumMemorySize());
    qDebug() << "ViewerCache disk size: " << printAsRAM(maxDiskCacheSize);
}

void AppManager::setApplicationsCachesMaximumDiskSpace(unsigned long long size){
    size_t maxCacheRAM = _imp->_settings->getRamMaximumPercent() * getSystemTotalRAM();
    U64 playbackSize = maxCacheRAM * _imp->_settings->getRamPlaybackMaximumPercent();
    _imp->_viewerCache->setMaximumCacheSize(size);
    _imp->_viewerCache->setMaximumInMemorySize((double)playbackSize / (double)size);
    
    qDebug() << "NodeCache RAM size: " << printAsRAM(_imp->_nodeCache->getMaximumMemorySize());
    qDebug() << "ViewerCache RAM size (playback-cache): " << printAsRAM(_imp->_viewerCache->getMaximumMemorySize());
    qDebug() << "ViewerCache disk size: " << printAsRAM(size);
}

void AppManager::setPlaybackCacheMaximumSize(double p){
    size_t maxCacheRAM = _imp->_settings->getRamMaximumPercent() * getSystemTotalRAM();
    U64 playbackSize = maxCacheRAM * p;
    _imp->_nodeCache->setMaximumCacheSize(maxCacheRAM - playbackSize);
    _imp->_nodeCache->setMaximumInMemorySize(1);
    U64 maxDiskCacheSize = _imp->_settings->getMaximumDiskCacheSize();
    _imp->_viewerCache->setMaximumInMemorySize((double)playbackSize / (double)maxDiskCacheSize);
    
    qDebug() << "NodeCache RAM size: " << printAsRAM(_imp->_nodeCache->getMaximumMemorySize());
    qDebug() << "ViewerCache RAM size (playback-cache): " << printAsRAM(_imp->_viewerCache->getMaximumMemorySize());
    qDebug() << "ViewerCache disk size: " << printAsRAM(maxDiskCacheSize);
}

void AppManager::loadAllPlugins() {
    assert(_imp->_plugins.empty());
    assert(_imp->_formats.empty());
    
    
    std::map<std::string,std::vector<std::string> > readersMap,writersMap;
    
    /*loading node plugins*/
    
    loadNodePlugins(&readersMap,&writersMap);
    
    /*loading ofx plugins*/
    _imp->ofxHost->loadOFXPlugins(&_imp->_plugins,&readersMap,&writersMap);
    
    _imp->_settings->populateReaderPluginsAndFormats(readersMap);
    _imp->_settings->populateWriterPluginsAndFormats(writersMap);
    
}

void AppManager::loadNodePlugins(std::map<std::string,std::vector<std::string> >* readersMap,
                                 std::map<std::string,std::vector<std::string> >* writersMap){
    std::vector<std::string> functions;
    functions.push_back("BuildEffect");
    std::vector<LibraryBinary*> plugins = AppManager::loadPluginsAndFindFunctions(NATRON_NODES_PLUGINS_PATH, functions);
    for (U32 i = 0 ; i < plugins.size(); ++i) {
        std::pair<bool,EffectBuilder> func = plugins[i]->findFunction<EffectBuilder>("BuildEffect");
        if(func.first){
            EffectInstance* effect = func.second(NULL);
            assert(effect);
            QMutex* pluginMutex = NULL;
            if(effect->renderThreadSafety() == Natron::EffectInstance::UNSAFE){
                pluginMutex = new QMutex(QMutex::Recursive);
            }
            Natron::Plugin* plugin = new Natron::Plugin(plugins[i],effect->pluginID().c_str(),effect->pluginLabel().c_str(),
                                                        pluginMutex,effect->majorVersion(),effect->minorVersion());
            _imp->_plugins.push_back(plugin);
            delete effect;
        }
    }
    
    loadBuiltinNodePlugins(&_imp->_plugins,readersMap,writersMap);
}

void AppManager::loadBuiltinNodePlugins(std::vector<Natron::Plugin*>* /*plugins*/,
                                        std::map<std::string,std::vector<std::string> >* /*readersMap*/,
                                        std::map<std::string,std::vector<std::string> >* /*writersMap*/){
}



void AppManagerPrivate::loadBuiltinFormats(){
    /*initializing list of all Formats available*/
    std::vector<std::string> formatNames;
    formatNames.push_back("PC_Video");
    formatNames.push_back("NTSC");
    formatNames.push_back("PAL");
    formatNames.push_back("HD");
    formatNames.push_back("NTSC_16:9");
    formatNames.push_back("PAL_16:9");
    formatNames.push_back("1K_Super_35(full-ap)");
    formatNames.push_back("1K_Cinemascope");
    formatNames.push_back("2K_Super_35(full-ap)");
    formatNames.push_back("2K_Cinemascope");
    formatNames.push_back("4K_Super_35(full-ap)");
    formatNames.push_back("4K_Cinemascope");
    formatNames.push_back("square_256");
    formatNames.push_back("square_512");
    formatNames.push_back("square_1K");
    formatNames.push_back("square_2K");
    
    std::vector< std::vector<double> > resolutions;
    std::vector<double> pcvideo; pcvideo.push_back(640); pcvideo.push_back(480); pcvideo.push_back(1);
    std::vector<double> ntsc; ntsc.push_back(720); ntsc.push_back(486); ntsc.push_back(0.91f);
    std::vector<double> pal; pal.push_back(720); pal.push_back(576); pal.push_back(1.09f);
    std::vector<double> hd; hd.push_back(1920); hd.push_back(1080); hd.push_back(1);
    std::vector<double> ntsc169; ntsc169.push_back(720); ntsc169.push_back(486); ntsc169.push_back(1.21f);
    std::vector<double> pal169; pal169.push_back(720); pal169.push_back(576); pal169.push_back(1.46f);
    std::vector<double> super351k; super351k.push_back(1024); super351k.push_back(778); super351k.push_back(1);
    std::vector<double> cine1k; cine1k.push_back(914); cine1k.push_back(778); cine1k.push_back(2);
    std::vector<double> super352k; super352k.push_back(2048); super352k.push_back(1556); super352k.push_back(1);
    std::vector<double> cine2K; cine2K.push_back(1828); cine2K.push_back(1556); cine2K.push_back(2);
    std::vector<double> super4K35; super4K35.push_back(4096); super4K35.push_back(3112); super4K35.push_back(1);
    std::vector<double> cine4K; cine4K.push_back(3656); cine4K.push_back(3112); cine4K.push_back(2);
    std::vector<double> square256; square256.push_back(256); square256.push_back(256); square256.push_back(1);
    std::vector<double> square512; square512.push_back(512); square512.push_back(512); square512.push_back(1);
    std::vector<double> square1K; square1K.push_back(1024); square1K.push_back(1024); square1K.push_back(1);
    std::vector<double> square2K; square2K.push_back(2048); square2K.push_back(2048); square2K.push_back(1);
    
    resolutions.push_back(pcvideo);
    resolutions.push_back(ntsc);
    resolutions.push_back(pal);
    resolutions.push_back(hd);
    resolutions.push_back(ntsc169);
    resolutions.push_back(pal169);
    resolutions.push_back(super351k);
    resolutions.push_back(cine1k);
    resolutions.push_back(super352k);
    resolutions.push_back(cine2K);
    resolutions.push_back(super4K35);
    resolutions.push_back(cine4K);
    resolutions.push_back(square256);
    resolutions.push_back(square512);
    resolutions.push_back(square1K);
    resolutions.push_back(square2K);
    
    assert(formatNames.size() == resolutions.size());
    for(U32 i =0;i<formatNames.size();++i) {
        const std::vector<double>& v = resolutions[i];
        assert(v.size() >= 3);
        Format* _frmt = new Format(0, 0, (int)v[0], (int)v[1], formatNames[i], v[2]);
        assert(_frmt);
        _formats.push_back(_frmt);
    }
    
}



Format* AppManager::findExistingFormat(int w, int h, double pixel_aspect) const {
    
    for(U32 i =0;i< _imp->_formats.size();++i) {
        Format* frmt = _imp->_formats[i];
        assert(frmt);
        if(frmt->width() == w && frmt->height() == h && frmt->getPixelAspect() == pixel_aspect){
            return frmt;
        }
    }
    return NULL;
}




void AppManager::setAsTopLevelInstance(int appID){
    if(_imp->_topLevelInstanceID == appID){
        return;
    }
    _imp->_topLevelInstanceID = appID;
    for(std::map<int,AppInstance*>::iterator it = _imp->_appInstances.begin();it!=_imp->_appInstances.end();++it){
        if (it->first != _imp->_topLevelInstanceID) {
            if(!isBackground())
                it->second->disconnectViewersFromViewerCache();
        }else{
            if(!isBackground())
                it->second->connectViewersToViewerCache();
        }
    }
}


void AppManager::clearExceedingEntriesFromNodeCache(){
    _imp->_nodeCache->clearExceedingEntries();
}

QStringList AppManager::getNodeNameList() const{
    QStringList ret;
    for (U32 i = 0; i < _imp->_plugins.size(); ++i) {
        ret.append(_imp->_plugins[i]->getPluginID());
    }
    return ret;
}

QMutex* AppManager::getMutexForPlugin(const QString& pluginId) const {
    for (U32 i = 0; i < _imp->_plugins.size(); ++i) {
        if(_imp->_plugins[i]->getPluginID() == pluginId){
            return _imp->_plugins[i]->getPluginLock();
        }
    }
    std::string exc("Couldn't find a plugin named ");
    exc.append(pluginId.toStdString());
    throw std::invalid_argument(exc);
}

const std::vector<Format*>& AppManager::getFormats() const {
    return _imp->_formats;
}

const KnobFactory& AppManager::getKnobFactory() const {
    return *(_imp->_knobFactory);
}

Natron::LibraryBinary* AppManager::getPluginBinary(const QString& pluginId,int majorVersion,int minorVersion) const{
    std::map<int,Natron::Plugin*> matches;
    for (U32 i = 0; i < _imp->_plugins.size(); ++i) {
        if(_imp->_plugins[i]->getPluginID() != pluginId){
            continue;
        }
        if(majorVersion != -1 && _imp->_plugins[i]->getMajorVersion() != majorVersion){
            continue;
        }
        matches.insert(std::make_pair(_imp->_plugins[i]->getMinorVersion(),_imp->_plugins[i]));
    }
    
    if(matches.empty()){
        QString exc = QString("Couldn't find a plugin named %1, with a major version of %2 and a minor version greater or equal to %3.")
                .arg(pluginId)
                .arg(majorVersion)
                .arg(minorVersion);
        throw std::invalid_argument(exc.toStdString());
    }else{
        std::map<int,Natron::Plugin*>::iterator greatest = matches.end();
        --greatest;
        return greatest->second->getLibraryBinary();
    }
}


Natron::EffectInstance* AppManager::createOFXEffect(const std::string& pluginID,Natron::Node* node) const {
    return _imp->ofxHost->createOfxEffect(pluginID, node);
}

void AppManager::removeFromNodeCache(boost::shared_ptr<Natron::Image> image){
    _imp->_nodeCache->removeEntry(image);
    if(image) {
        emit imageRemovedFromNodeCache(image->getKey()._time);
    }
}

void AppManager::removeFromViewerCache(boost::shared_ptr<Natron::FrameEntry> texture){
    _imp->_viewerCache->removeEntry(texture);
    if(texture) {
        emit imageRemovedFromNodeCache(texture->getKey()._time);
    }
}

const QString& AppManager::getApplicationBinaryPath() const {
    return _imp->_binaryPath;
}

void AppManager::setMultiThreadEnabled(bool enabled) {
    _imp->_settings->setMultiThreadingDisabled(!enabled);
}

bool AppManager::getImage(const Natron::ImageKey& key,boost::shared_ptr<Natron::Image>* returnValue) const {
#ifdef NATRON_LOG
    Log::beginFunction("AppManager","getImage");
    bool ret = _imp->_nodeCache->get(key, returnValue);
    if(ret) {
        Log::print("Image found in cache!");
    } else {
        Log::print("Image not found in cache!");
    }
    Log::endFunction("AppManager","getImage");
#else
    return _imp->_nodeCache->get(key, returnValue);
#endif

}

bool AppManager::getTexture(const Natron::FrameKey& key,boost::shared_ptr<Natron::FrameEntry>* returnValue) const {
#ifdef NATRON_LOG
    Log::beginFunction("AppManager","getTexture");
    bool ret = _imp->_viewerCache->get(key, returnValue);
    if(ret) {
        Log::print("Texture found in cache!");
    } else {
        Log::print("Texture not found in cache!");
    }
    Log::endFunction("AppManager","getTexture");
#else
    return _imp->_viewerCache->get(key, returnValue);
#endif
}

U64 AppManager::getCachesTotalMemorySize() const {
    return _imp->_viewerCache->getMemoryCacheSize() + _imp->_nodeCache->getMemoryCacheSize();
}

Natron::CacheSignalEmitter* AppManager::getOrActivateViewerCacheSignalEmitter() const {
    return _imp->_viewerCache->activateSignalEmitter();
}

boost::shared_ptr<Settings> AppManager::getCurrentSettings() const {
    return _imp->_settings;
}

void AppManager::setLoadingStatus(const QString& str) {
    if (isLoaded()) {
        return;
    }
    std::cout << str.toStdString() << std::endl;
}

AppInstance* AppManager::makeNewInstance(int appID) const {
    return new AppInstance(appID);
}

#if QT_VERSION < 0x050000
Q_DECLARE_METATYPE(QAbstractSocket::SocketState)
#endif

void AppManager::registerEngineMetaTypes() const {
    qRegisterMetaType<Variant>();
    qRegisterMetaType<Natron::ChannelSet>();
    qRegisterMetaType<Format>();
    qRegisterMetaType<SequenceTime>("SequenceTime");
    qRegisterMetaType<Knob*>();
    qRegisterMetaType<Natron::Node*>();
    qRegisterMetaType<Natron::StandardButtons>();
#if QT_VERSION < 0x050000
    qRegisterMetaType<QAbstractSocket::SocketState>("SocketState");
#endif
}


namespace Natron{

void errorDialog(const std::string& title,const std::string& message){
    appPTR->hideSplashScreen();
    AppInstance* topLvlInstance = appPTR->getTopLevelInstance();
    assert(topLvlInstance);
    if(!appPTR->isBackground()){
        topLvlInstance->errorDialog(title,message);
    }else{
        std::cout << "ERROR: " << message << std::endl;
    }

#ifdef NATRON_LOG
    Log::beginFunction(title,"ERROR");
    Log::print(message);
    Log::endFunction(title,"ERROR");
#endif
}

void warningDialog(const std::string& title,const std::string& message){
    appPTR->hideSplashScreen();
    AppInstance* topLvlInstance = appPTR->getTopLevelInstance();
    assert(topLvlInstance);
    if(!appPTR->isBackground()){
        topLvlInstance->warningDialog(title,message);
    }else{
        std::cout << "WARNING: "<< message << std::endl;
    }
#ifdef NATRON_LOG
    Log::beginFunction(title,"WARNING");
    Log::print(message);
    Log::endFunction(title,"WARNING");
#endif
}

void informationDialog(const std::string& title,const std::string& message){
    appPTR->hideSplashScreen();
    AppInstance* topLvlInstance = appPTR->getTopLevelInstance();
    assert(topLvlInstance);
    if(!appPTR->isBackground()){
        topLvlInstance->informationDialog(title,message);
    }else{
        std::cout << "INFO: "<< message << std::endl;
    }
#ifdef NATRON_LOG
    Log::beginFunction(title,"INFO");
    Log::print(message);
    Log::endFunction(title,"INFO");
#endif
}

Natron::StandardButton questionDialog(const std::string& title,const std::string& message,Natron::StandardButtons buttons,
                                      Natron::StandardButton defaultButton){
    appPTR->hideSplashScreen();
    AppInstance* topLvlInstance = appPTR->getTopLevelInstance();
    assert(topLvlInstance);
    if(!appPTR->isBackground()){
        return topLvlInstance->questionDialog(title,message,buttons,defaultButton);
    }else{
        std::cout << "QUESTION ASKED: " << message << std::endl;
        std::cout << NATRON_APPLICATION_NAME " answered yes." << std::endl;
        return Natron::Yes;
    }
#ifdef NATRON_LOG
    Log::beginFunction(title,"QUESTION");
    Log::print(message);
    Log::endFunction(title,"QUESTION");
#endif
}



} //Namespace Natron