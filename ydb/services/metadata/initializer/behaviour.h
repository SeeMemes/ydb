#pragma once

#include <ydb/services/metadata/abstract/initialization.h>
#include <ydb/services/metadata/abstract/kqp_common.h>
#include <ydb/services/metadata/manager/common.h>

namespace NKikimr::NMetadata::NInitializer {

class TDBObjectBehaviour: public IClassBehaviour {
protected:
    virtual IInitializationBehaviour::TPtr ConstructInitializer() const override;
    virtual std::shared_ptr<NModifications::IOperationsManager> ConstructOperationsManager() const override;

    virtual TString GetInternalStorageTablePath() const override;
    virtual TString GetInternalStorageHistoryTablePath() const override {
        return "";
    }

    virtual TString GetTypeId() const override;
};

}
