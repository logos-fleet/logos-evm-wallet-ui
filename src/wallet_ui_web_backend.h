#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

#include "rep_wallet_ui_source.h"

// Declared, not included: the door itself (logos_web_module_call.h) stays the
// .cpp's business, and a helper that reports a refusal only needs to name the
// reply type it is handed.
namespace logos::web {
struct ModuleCallResult;
}

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
// is a `core` module with five dependencies of its own and no mobile Bare build;
// `eth_rpc_module` and `uniswap_module` do have one, and a balance or a price is
// one call to the module that owns it. So the `web` variant is the wallet's thin
// half, and it asks each module directly rather than through the coordinator:
// accounts out of `keystore_module` (itself a `web` variant on a phone),
// balances out of the Bundled `eth_rpc_module`, prices out of the Bundled
// `uniswap_module` (#148). What the coordinator STILL owns alone — sends, fee
// estimation, history, token lists — REFUSES BY NAME rather than returning a
// plausible empty value, so a user is told the variant cannot do it and a
// developer is told which module is missing.
//
// GOING THROUGH THE COORDINATOR WOULD NOT BE CHEAPER. `wallet_backend_module`'s
// closure names all five of the wallet's modules, so a Bundled set that carried
// it and not them could not satisfy it — which is why each module the phone has
// is reached by name, and why this file grows one section per module ported
// rather than shrinking to one call.
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

    // Market — uniswap_module
    void refreshMarket(QString address) override;

    // Send
    QString estimateFee(QString sendJson) override;
    QString sendNative(QString sendJson) override;
    QString sendErc20(QString sendJson) override;

    // History
    void refreshHistory(QString address) override;

    // Private (RAILGUN) — the accumulator sync a private send waits on.
    void refreshPrivateSync() override;
    QString startPrivateSync() override;
    QString cancelPrivateSync() override;

    // ...and the send the sync is a leg of.
    QString startPrivateSend(QString sendJson) override;
    QString cancelPrivateSend() override;

    // ...and the other direction: public funds going INTO the pool.
    QString startPrivateShield(QString shieldJson) override;
    QString cancelPrivateShield() override;

private:
    // THE VARIANT STARTS ITSELF. There is no onContextReady in a wasm image:
    // the host constructs this object in main() and the page's bridge is not
    // bound yet, so the first outbound call has to wait for a channel that
    // appears later. This timer is that wait — it polls the door until it
    // opens, then asks the keystore for accounts once and stops.
    QTimer m_startup;
    int m_startupTicks = 0;
    // Attempts each first ask has spent on the admission race, counted up to
    // kAdmissionRetries and reset once that ask has been answered.
    int m_accountRetries = 0;
    int m_privateSyncRetries = 0;

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

    // The market fan-out, the same single-slot-plus-epoch shape as the balance
    // one above and for the same reasons — one account at a time, a second ask
    // supersedes the first, and a reply from a superseded fan-out is dropped
    // rather than merged into the one that replaced it.
    quint64 m_marketEpoch = 0;
    int m_marketPending = 0;
    QJsonObject m_market;

    // The Tokens tab's read, epoch'd for the same reason the two fan-outs above
    // are even though it is a single call: the tab shows ONE chain at a time, so
    // a reply for a chain the user has navigated away from — or the re-read a
    // custom-token add kicks off — must not repaint it under the newer ask.
    quint64 m_tokensEpoch = 0;

    // The last native balance seen per chain, in WEI, as `fetchBalance` read it
    // off eth_rpc — kept so the Market tab can show a value and not only a
    // price. A CACHE OF AN OBSERVATION, never a source of truth: a chain that
    // has not answered a balance yet is simply absent, and the market item for
    // it then carries no `valueUsd`, which is exactly what the view renders as
    // blank. (Refreshing the market does not fetch balances: the two tabs are
    // separate asks, and a market refresh that silently re-read every balance
    // would spend a chain's RPC budget twice for one number.)
    QHash<int, QString> m_nativeWei;

    // The chains `eth_rpc_module` has been told about in this page's lifetime.
    // A `web` variant's page dies with the app, and eth_rpc's config is the
    // NATIVE module's, so this only avoids re-sending — it is a cache, never a
    // source of truth, and a chain eth_rpc refused drops back out of it.
    QSet<int> m_chainsConfigured;

    // ── the private sync, walked ─────────────────────────────────────────────
    //
    // A SYNC IS A WALK, NOT A CALL. `railgun_module.sync()` is one call that
    // took 221 s on a physical iPad Air 4 with nothing to report and nothing to
    // interrupt (logos-workspace#235); `sync_step` is the same work in windows.
    // So the state a walk needs lives here: whether one is in flight, whether
    // the user has left, and what the last window said.
    //
    // THE NEXT WINDOW IS ASKED FOR FROM INSIDE THE LAST ONE'S REPLY, which is
    // the same rule every chained call in this file follows and here it is also
    // what makes a cancel possible: there is never more than one window in
    // flight, so "stop" is "do not ask again".
    bool m_syncRunning = false;
    bool m_syncCancelled = false;
    // WHAT THE WALK IS FOR, when it is for something. A walk the user asked for
    // on the Private tab ends and that is all; a walk a SEND asked for is that
    // send's first leg and has to hand over to the second. Set by
    // startPrivateSend, run once at whichever end the walk reaches (finished,
    // stalled, refused or cancelled) and cleared before it runs, so the two
    // paths that can end a walk at the same moment — the cancel's own reply and
    // the window that was already in flight — cannot both carry the send on.
    std::function<void(bool, const QString&)> m_syncThen;
    // The last plan `railgun_module` answered with, kept so a cancel can report
    // the block the walk reached without asking for it a second time.
    QJsonObject m_syncPlan;

    void stepPrivateSync();
    // The walk has reached an end. Runs `m_syncThen` at most once; a walk
    // nobody is waiting on simply has none.
    void syncLegEnded(bool ok, const QString& why);
    // Publish `privateSyncJson`. `state` is the verdict this variant puts on
    // the module's plan (idle / running / done / cancelled / unavailable);
    // `plan` is the module's own fields, passed through unchanged so the view
    // reads the numbers the engine produced rather than a second copy of them.
    void publishPrivateSync(const QString& state, const QJsonObject& plan,
                            const QString& note = QString(),
                            const QString& error = QString());
    // What a refused `railgun_module` call publishes: `unavailable`, with the
    // module's own words or the door's. A build that ships no railgun_module at
    // all lands here too, which is the honest answer for it — this variant
    // does not declare railgun a dependency (see the .cpp) precisely so that
    // build still loads.
    QString privateSyncUnavailable(const QString& method,
                                   const logos::web::ModuleCallResult& res,
                                   const QJsonObject& reply);

    // ── the private send, run as its legs ────────────────────────────────────
    //
    // ONE CALL IN FLIGHT AT A TIME, all the way down the route, for the same
    // reason the walk chains its windows: a route where exactly one thing is
    // outstanding is a route where "cancel" has one meaning, and the meaning is
    // "do not make the next call". The legs are `sync` (the walk above),
    // `prove` (`relayed_send`: the 7702 UserOp, its Groth16 proof, and an
    // approval request lodged with keystore_module), `approve` (a human in the
    // Signer app, polled for) and `broadcast` (the approved operation submitted
    // to the bundler through eth_rpc).
    //
    // WHAT A CANCEL COSTS AT EACH LEG, which is what #235 asks to be decided
    // and documented rather than discovered: during `sync` nothing has been
    // sent and every window the walk finished is persisted; during `prove` and
    // `approve` nothing has been signed and the request is withdrawn from the
    // approver's queue; after `broadcast` there is nothing to cancel and the
    // ask is REFUSED. A shield that was mined before any of this is untouched
    // throughout — it is the chain's, owned by this wallet's 0zk address.
    bool m_sendRunning = false;
    bool m_sendCancelled = false;
    // The leg running now, and every leg with its own state. `m_sendLeg` is a
    // convenience for a view that shows one line; `m_sendLegs` is the route,
    // and is what makes "skipped" distinguishable from "done".
    QString m_sendLeg;
    QJsonArray m_sendLegs;
    // The approval request `relayed_send` lodged, kept because a withdrawal
    // names it and because a view showing a pending send should be able to.
    QString m_sendRequestId;
    QString m_sendUserOpHash;
    // The parameters this send was started with, normalised — held because the
    // prove leg is issued after the walk, which is minutes later.
    QJsonObject m_sendParams;

    // Lay out a fresh route: four legs, all pending.
    void resetSendRoute();
    void setSendLeg(const QString& leg, const QString& state);
    QString sendLegState(const QString& leg) const;
    // The prove leg: `relayed_send`, whose reply is the approval request's id.
    void beginSendProve();
    // The approve leg: `relayed_send_status`, until a human has decided. The
    // poll that answers `done` is also the call that submitted the operation,
    // so the broadcast leg is reported by its RESULT and never as running —
    // this side cannot see it start.
    void pollSendApproval();
    // A leg that could not go on: the console, the status line, the leg and the
    // send all say the same thing, which is why they are said in one place.
    void failSendLeg(const QString& leg, const QString& why);
    // Take the approval request out of the Signer's queue and end the send.
    void withdrawSendRequest();
    void publishPrivateSend(const QString& state, const QString& note = QString(),
                            const QString& error = QString());
    // The send is over, however it ended. The approval poll stops on its own:
    // its timer checks `m_sendRunning` before it asks again.
    void finishSend(const QString& state, const QString& note = QString(),
                    const QString& error = QString());

    // ── the shield, run as its legs ──────────────────────────────────────────
    //
    // THE ROUTE INTO THE POOL, which is the half the send above cannot do
    // without: `plan` (`prepare_shield` for the calldata, plus the account's
    // nonce and the chain's gas price), `sign` (ONE approval request carrying
    // every transaction of the route, answered once by a human in the Signer
    // app), then `wrap`, `approve` and `shield` — each broadcast and then
    // followed to its receipt.
    //
    // WHY ONE BUNDLE AND NOT THREE ASKS. `keystore_module` signs an intent's
    // legs in order under a SINGLE key derivation, so the whole route is shown
    // to the human together and costs one password entry. The legs are signed
    // with CONSECUTIVE NONCES, so the chain executes the allowance before the
    // shield without this wallet waiting for a receipt in between — which is
    // what lets all three go out back to back and keeps the wait one wait.
    //
    // WHERE THE CANCEL LINE IS, and it is a harder line than the send's: up to
    // the moment the first raw transaction is handed to eth_rpc there is
    // nothing on chain and the request is withdrawn from the Signer's queue.
    // After it, the transactions are the chain's — nonce-ordered and already
    // in a mempool — and a cancel is REFUSED rather than pretended.
    bool m_shieldRunning = false;
    bool m_shieldCancelled = false;
    // Flipped as the FIRST transaction is handed over, not as the last one is
    // acknowledged: the point of no return is the handover, and a flag set on
    // the reply would leave a window where a cancel answered "stopped" for
    // something already in a mempool.
    bool m_shieldBroadcast = false;
    QString m_shieldLeg;
    QJsonArray m_shieldLegs;
    // `{chainId, owner, asset, amount, wrap}` as startPrivateShield normalised
    // them — held because the route's later calls happen minutes later.
    QJsonObject m_shieldParams;
    // ONE ENTRY PER TRANSACTION, in the order they are signed and sent:
    // `{leg, to, data, value, nonce, gasLimit}`, and `hash` once it has been
    // broadcast. A leg can own more than one (`prepare_shield` answers an
    // array), so the route's legs are derived from this list rather than the
    // list being derived from the legs.
    QJsonArray m_shieldTxs;
    QString m_shieldHandle;
    QString m_shieldReceipt;
    QStringList m_shieldSigned;
    int m_shieldSent = 0;
    int m_shieldMined = 0;
    // The nonce the account was at when the route was planned, and the fee the
    // chain suggested — both read once, because every leg of one bundle has to
    // agree about them.
    quint64 m_shieldNonce = 0;
    quint64 m_shieldMaxFee = 0;
    quint64 m_shieldTip = 0;

    void resetShieldRoute();
    void setShieldLeg(const QString& leg, const QString& state);
    QString shieldLegState(const QString& leg) const;
    // The three reads the plan is made of, chained because two calls issued in
    // one turn are answered in whatever order the container finishes them (see
    // ensureChainConfig).
    void shieldReadNonce();
    void shieldReadGasPrice();
    // Lay the transactions out with their nonces and fees. Answers the reason
    // it could not, which is always something about the caller's own numbers.
    QString buildShieldTxs();
    // Put the whole bundle in front of a human, once.
    void requestShieldApproval();
    void pollShieldApproval();
    void collectShieldSignatures();
    // Hand the signed transactions to eth_rpc, in nonce order, without waiting
    // for a receipt between them.
    void broadcastShieldTx();
    // The signatures are spent; let the keystore wipe its copy.
    void ackShieldSignatures();
    // ...and then follow each transaction to the block it landed in, which is
    // the only part of this route whose length the chain decides.
    void followShieldReceipt();
    void withdrawShieldRequest();
    void failShieldLeg(const QString& leg, const QString& why);
    void publishPrivateShield(const QString& state, const QString& note = QString(),
                              const QString& error = QString());
    void finishShield(const QString& state, const QString& note = QString(),
                      const QString& error = QString());

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

    // Name this module a custodian on the keystore, then run `then`. The same
    // chained shape as ensureChainConfig and for the same reason: the role has to
    // be IN FORCE before the mutation is asked for, and two calls issued in one
    // turn are answered in whatever order the container finishes them. `then`
    // runs only when the role was taken — a refusal here means the mutation
    // would be refused too, and saying so once is better than saying it twice.
    void claimCustody(std::function<void()> then);
    // The last link of an import's chain: name the account, then re-read the
    // list. Split out of importMnemonic for the same reason takeCustodianRole is
    // split out of claimCustody — a third nested lambda is not a sequence a
    // reader can follow. A blank label skips straight to the list, and a refused
    // one is reported without unsaying "Account imported".
    void labelAccount(const QString& address, const QString& label,
                      const QString& password);
    // The second link of that chain: the `configure` call itself. Split out so
    // claimCustody reads as the two questions it asks — "who am I here?" and
    // "may I mutate?" — rather than as two nested lambdas.
    void takeCustodianRole(std::function<void()> then);
    // Announce and show a refused keystore call, in the two places every refusal
    // here goes, and report whether the caller should stop.
    bool keystoreRefused(const QString& method, const logos::web::ModuleCallResult& res,
                         const QJsonObject& reply);

    // The two things a configured chain is asked, and the only two continuations
    // ensureChainConfig is ever given.
    void verifyChain(int chainId);
    void fetchBalance(const QString& address, int chainId, const QString& symbol);
    void publishBalances();
    void publishChains();

    // The market's counterpart of the pair above: the continuation
    // ensureChainConfig is given by refreshMarket, and the publish its replies
    // count down to. `fetchPrices` needs eth_rpc configured for the chain even
    // though it asks UNISWAP — uniswap's every price is one Multicall3
    // `eth_call` issued through eth_rpc, so an unconfigured chain comes back as
    // uniswap reporting eth_rpc's own refusal.
    void fetchPrices(int chainId, const QString& symbol);
    void publishMarket();
    // The `prices` array uniswap answered, in the shape the view's `marketJson`
    // carries: one item per asset, labelled with the chain's `nativeSymbol`
    // where the entry is the native asset, and carrying `valueUsd` where a
    // holding is known. Named rather than written inline in `fetchPrices`'s
    // reply, where it would be a third level of nesting inside a lambda.
    QJsonArray priceItems(const QJsonArray& prices, int chainId,
                          const QString& symbol) const;
    // What the account's holding of this chain's NATIVE asset is worth, from the
    // wei `fetchBalance` last saw and the USD price uniswap just answered. Null
    // — and the view's value column blank — for anything it cannot honestly
    // multiply: a price entry that is not the native asset (no balance is known
    // for a token here), a chain that has answered no balance yet, or a price
    // uniswap could not anchor in USD.
    QJsonValue nativeValueUsd(int chainId, const QJsonObject& price) const;

    // What every method this variant does not implement answers with. Names the
    // module that would have served it, so the refusal is diagnosable.
    QString refuse(const QString& what, const QString& module);
};
