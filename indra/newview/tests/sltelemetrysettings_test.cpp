// Author: SkyLine. SPDX-License-Identifier: LGPL-2.1-only
// Links actual LLControlGroup/LLSD/file code; no viewer, listener or account data.
#include "linden_common.h"
#include "llcontrol.h"
#include "llsdserialize.h"
#include "llapp.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
int main()
{
    const auto root=fs::temp_directory_path()/"sltelemetry-settings-offline-test";
    fs::remove_all(root);fs::create_directory(root);
    const auto path=(root/"settings.xml").string();
    LLControlGroup settings("TelemetryPrivacyFixture");
    settings.declareS32("SLTelemetryConsent",0,"fixture");
    auto* pending=settings.declareBOOL("SLTelemetryPurgePending",false,"fixture");
    pending->setHiddenFromSettingsEditor(true);
    pending->setPersist(LLControlVariable::PERSIST_ALWAYS);
    assert(pending->isHiddenFromSettingsEditor());
    for (S32 consent:{0,-1,1})
    {
        settings.setS32("SLTelemetryConsent",consent);
        for (bool revoked:{true,false})
        {
            settings.setBOOL("SLTelemetryPurgePending",revoked);
            assert(settings.saveToFile(path,true)>0);
            LLControlGroup restored("TelemetryPrivacyRestored");
            restored.declareS32("SLTelemetryConsent",0,"fixture");
            restored.declareBOOL("SLTelemetryPurgePending",false,"fixture");
            assert(restored.loadFromFile(path)>0);
            assert(restored.getS32("SLTelemetryConsent")==consent);
            assert(restored.getBOOL("SLTelemetryPurgePending")==revoked);
        }
    }
    settings.setBOOL("SLTelemetryPurgePending",true);
    assert(settings.saveToFile((root/"absent"/"settings.xml").string(),true)==0); // open failure
    const auto directory=root/"destination-directory";fs::create_directory(directory);
    assert(settings.saveToFile(directory.string(),true)==0); // replace failure
#ifdef LL_LINUX
    const auto temporary=path+"."+std::to_string(LLApp::getPid())+".tmp";
    fs::create_symlink("/dev/full",temporary);
    std::ofstream openProbe(temporary,std::ios::binary|std::ios::trunc);
    if (openProbe.is_open())
    {
        openProbe.close();
        assert(settings.saveToFile(path,true)==0); // actual write/close failure, open verified
        std::cout<<"PASS actual /dev/full write/close failure (open verified)\n";
    }
    else std::cout<<"UNSUPPORTED /dev/full write/close fixture: sandbox denied open\n";
    fs::remove(temporary);
#endif
    assert(settings.getBOOL("SLTelemetryPurgePending"));
    fs::remove_all(root);
}
