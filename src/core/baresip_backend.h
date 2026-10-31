#pragma once

#include "baresip_client.h"

#include <QString>

namespace leansip {

// What account validation needs from the thing that runs baresip. BaresipProcess (direct child)
// and DaemonBackend (leansipd) implement it; tests substitute a fake in-process server.
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

    // Around restart(): a client talking to baresip directly must drop and re-establish its
    // connection. Through leansipd the client's connection survives, so those are no-ops there.
    virtual void detachClient(BaresipClient *client) { client->disconnectFromHost(); }
    virtual void attachClient(BaresipClient *client) { client->connectToHost(host(), port(), 5000); }
};

} // namespace leansip
