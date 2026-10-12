#pragma once

#include <QString>

namespace leansip {

// What account validation needs from the thing that runs baresip. BaresipProcess implements it;
// tests substitute a fake that just points at an in-process fake server.
class BaresipBackend {
public:
    virtual ~BaresipBackend() = default;

    // Stops and starts baresip again; the control port may change. False on failure.
    virtual bool restart() = 0;
    virtual QString host() const = 0;
    virtual quint16 port() const = 0;

    // Path of the accounts file the *running* baresip reads (inside the derived dir).
    virtual QString derivedAccountsPath() const = 0;
    // While true, restart() leaves derivedAccountsPath() alone instead of re-linking the user's file.
    virtual void setAccountsOverride(bool enabled) = 0;
    virtual QString lastError() const = 0;
};

} // namespace leansip
