#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>

#include "rep_wallet_ui_source.h"

// THE WALLET UI'S `web` VARIANT BACKEND — the same `.rep`, the same QML, a
// different way of reaching everything behind it (slice 30, criterion 2).
//
// WHY IT IS A SECOND CLASS AND NOT AN #ifdef IN wallet_ui_backend.cpp. The
// desktop backend reaches `wallet_backend_module` through generated typed
// clients over LogosAPI, and every one of those calls is SYNCHRONOUS:
//
//     setChainsJson(modules().wallet_backend_module.get_chains());
//
// A wasm image is single-threaded with no ASYNCIFY (ADR 0004). Its replies
// arrive as messages on the page's event loop, so a call that blocked waiting
// for one would deadlock the loop that is meant to deliver it. Not one line of
// the desktop backend can be reused, because its shape — value in, value out —
// is the thing the target forbids. The two files share the contract (the .rep)
// and nothing else, which is the honest amount.
//
// WHAT IT TALKS TO, and why it is not `wallet_backend_module`. The coordinator
// is a `core` module with five dependencies of its own, none of which has a
// mobile Bare build; `eth_rpc_module` does, and a balance is one call to it.
// So the `web` variant is the wallet's thin half: accounts out of
// `keystore_module` (itself a `web` variant on a phone) and balances straight
// out of the Bundled `eth_rpc_module`. Everything the coordinator owns —
// sends, the market, history — REFUSES BY NAME rather than returning
// a plausible empty value, so a user is told the variant cannot do it and a
// developer is told which module is missing.
//
// ONE DOOR OUT: logos::web::callModuleAsync (logos_web_module_call.h), which
// only exists inside the Wasm host. This header is therefore only ever
// compiled by logos_wasm_view_module(); the desktop plugin does not see it.
class WalletUiWebBackend : public WalletUiSimpleSource
{
    Q_OBJECT

public:
    explicit WalletUiWebBackend(QObject* parent = nullptr);

public slots:
    // Config / privacy
    bool setProxyConfig(QString proxyJson) override;
    bool setChains(QString chainsJson) override;
    QString testEndpoint(int chainId) override;

    // Accounts — keystore_module
    QString createAccount(QString passphrase, QString label) override;
    QString importMnemonic(QString phraseJson, QString label) override;
    void refreshAccounts() override;

    // A signing request the wallet raised, polled by the view until a human
    // answers it in the signer UI. This variant never raises one — it cannot
    // send — so it refuses by name like the rest of the coordinator's surface.
    QString sendStatus(QString requestId) override;

    // Balances — eth_rpc_module
    void refreshBalances(QString address) override;
    void loadTokens(int chainId) override;
    bool addCustomToken(QString tokenJson) override;

    // Market
    void refreshMarket(QString address) override;

    // Send
    QString estimateFee(QString sendJson) override;
    QString sendNative(QString sendJson) override;
    QString sendErc20(QString sendJson) override;

    // History
    void refreshHistory(QString address) override;

private:
    // THE VARIANT STARTS ITSELF. There is no onContextReady in a wasm image:
    // the host constructs this object in main() and the page's bridge is not
    // bound yet, so the first outbound call has to wait for a channel that
    // appears later. This timer is that wait — it polls the door until it
    // opens, then asks the keystore for accounts once and stops.
    QTimer m_startup;
    int m_startupTicks = 0;
    int m_accountRetries = 0;

    // The chains this variant knows, in the shape the view's `chainsJson`
    // carries. Seeded in the constructor and replaced by setChains().
    QJsonArray m_chains;

    // One balance fan-out in flight: how many chains are still to answer, and
    // what has come back so far. A SINGLE slot rather than a map keyed by
    // address, because the view asks for one account at a time and a second
    // ask supersedes the first — an image with one event loop has no use for
    // two half-finished aggregates.
    //
    // The epoch is what makes "supersedes" true. Every ask bumps it and every
    // reply carries the one it was made under, so a reply from a fan-out that
    // has been replaced is dropped — including a replacement for the SAME
    // address, whose stale replies would otherwise decrement the new counter
    // and publish an aggregate that is still filling.
    quint64 m_balanceEpoch = 0;
    int m_balancePending = 0;
    QJsonObject m_balances;

    // The chains `eth_rpc_module` has been told about in this page's lifetime.
    // A `web` variant's page dies with the app, and eth_rpc's config is the
    // NATIVE module's, so this only avoids re-sending — it is a cache, never a
    // source of truth, and a chain eth_rpc refused drops back out of it.
    QSet<int> m_chainsConfigured;

    QJsonObject chainById(int chainId) const;
    void seedDefaultChains();
    void startWhenReachable();

    // Configure a chain on eth_rpc, then run `then` — and the CONTINUATION is
    // the point. Two calls made in the same turn are delivered in order and
    // answered in whatever order the container finishes them, because a call to
    // an in-process Bare target spins a nested event loop inside the capability
    // handshake, which runs the NEXT queued call to completion first. Measured
    // on an iPad Air 13-inch simulator: `set_chain_config(1, …)` and
    // `get_balance(1, …)` issued together, and the balance answered
    // `{"ok":false,"error":"no configuration for chain 1"}` — the endpoint
    // landed after the question that needed it. logos_web_module_call.h states
    // the rule ("a backend that needs a sequence chains it in the callbacks");
    // this is that chain.
    //
    // `then` runs exactly once, whether the configuration was already in place,
    // succeeded, or was refused: a balance asked of an unconfigured chain
    // reports eth_rpc's own error, which is worth more than silence.
    void ensureChainConfig(int chainId, const QString& endpoint,
                           std::function<void()> then = {});
    void fetchBalance(const QString& address, int chainId, const QString& symbol);
    void publishBalances();
    void publishChains();

    // What every method this variant does not implement answers with. Names the
    // module that would have served it, so the refusal is diagnosable.
    QString refuse(const QString& what, const QString& module);
};
