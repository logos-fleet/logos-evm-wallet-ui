#include "wallet_ui_web_backend.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <initializer_list>

// The only way out of a wasm image. See wallet_ui_web_backend.h for why this
// backend exists at all, and logos_web_module_call.h for what the door is.
#include "logos_web_module_call.h"

namespace {

// The module a balance comes from on a phone: a Bundled Bare module, in the
// app image, reached by name through the container.
const QString kEthRpc = QStringLiteral("eth_rpc_module");
// The module an account comes from: itself a `web` variant on a Store shell,
// which the container publishes by name exactly as it does a native one.
const QString kKeystore = QStringLiteral("keystore_module");
// The module a PRICE comes from, and a Bundled Bare module like eth_rpc rather
// than a `web` variant like the keystore: uniswap's every method is one
// Multicall3 `eth_call` issued synchronously through eth_rpc, and a wasm image
// has no outbound door to issue it from (logos-protocol's wasm subset links no
// lp_client_create). So uniswap crosses to a phone as native machine code and
// this image calls it by name — see logos-evm-uniswap-module's flake for the
// whole of that argument.
const QString kUniswap = QStringLiteral("uniswap_module");
// The module a TOKEN LIST comes from, and a Bundled Bare module for the same
// kind of reason uniswap is — but a blunter one. token_list_module declares
// `platform: true` (ADR 0009): it builds its own HTTP client with `socks` and a
// `proxyRequired` that fails CLOSED, which is access a webview cannot give a
// page and cannot be ported to `fetch` without silently voiding that guarantee.
// So it crosses as native machine code and this image calls it by name.
const QString kTokenList = QStringLiteral("token_list_module");
// THE COORDINATOR, AND IT IS HERE AFTER ALL (logos-workspace#250). This file
// used to say `wallet_backend_module` had no mobile build and refuse its whole
// surface without asking. That stopped being true at #183, which gave it a
// mobile Bare build and a catalog entry: on the operator's iPad it was Bundled
// into the same app image as this page, and three screens still reported it
// missing -- because this variant neither declared it nor called it, so the core
// never loaded it and nothing ever asked it anything.
//
// IT IS AN OPTIONAL DEPENDENCY, NOT A REQUIRED ONE (metadata.json:
// `web.optional_dependencies`). An app image carries it only when `--bundle`
// asked for it, and the core refuses a module whose REQUIRED list it cannot
// satisfy -- so declaring it hard would trade every wallet build without the
// coordinator for the tabs below. Optional says exactly what is true: load it
// if it is here, and this page finds out by asking.
const QString kWalletBackend = QStringLiteral("wallet_backend_module");
// What uniswap puts in a price's `address` field for a chain's NATIVE asset;
// every other entry carries a real ERC-20 address. Not a display name — the
// chain list's `nativeSymbol` is that, and is what the item is labelled with.
const QString kNativeAsset = QStringLiteral("ETH");

// The History tab with nothing in it, spelled once. The view reads
// `historyJson.history`, and a tab emptied by a refusal and one emptied by a
// wallet that has sent nothing are the same document.
const QString kEmptyHistory = QStringLiteral("{\"history\":[]}");

// THE FIVE WORDS `privateSyncJson.sync.state` IS EVER ONE OF, spelled once.
// They are a contract, not a message: WalletView.qml colours the state label by
// them and docs/specs.md lists them, so a typo here is a state nothing renders
// and nothing complains about. Naming them also keeps them apart from the
// module's own `done` FIELD, which is spelled the same and means something
// else. See "the private sync" section near the bottom of this file.
const QString kStateIdle = QStringLiteral("idle");
const QString kStateRunning = QStringLiteral("running");
const QString kStateDone = QStringLiteral("done");
const QString kStateCancelled = QStringLiteral("cancelled");
const QString kStateUnavailable = QStringLiteral("unavailable");
// ...AND THE THREE MORE A LEG OF A SEND IS EVER IN. The same vocabulary
// deliberately: `privateSendJson.send.state` and every entry in its `legs` are
// read by the same QML colour function as the sync's, so a fifth spelling of
// "this went wrong" would be a state that renders as nothing.
// `skipped` is not `done` — a leg that was not needed and a leg that was run
// are different claims, and only one of them is work this wallet did.
const QString kStatePending = QStringLiteral("pending");
const QString kStateSkipped = QStringLiteral("skipped");
const QString kStateFailed = QStringLiteral("failed");

// THE ROUTE A PRIVATE SEND TAKES, in order. Named here because the order is the
// contract: the view renders it top to bottom and the backend walks it one leg
// at a time.
const QString kLegSync = QStringLiteral("sync");
const QString kLegProve = QStringLiteral("prove");
const QString kLegApprove = QStringLiteral("approve");
const QString kLegBroadcast = QStringLiteral("broadcast");

// How often the startup wait asks whether the page has a channel yet, and how
// long it waits before saying it has not. A minute: a cold phone launch mounts
// a webview, fetches a document and instantiates a ~26 MB runtime beside this
// image before the container's bridge resolves.
constexpr int kStartupPollMs = 100;
constexpr int kStartupGiveUpMs = 60000;

// HOW LONG AFTER ADMISSION THE FIRST OUTBOUND CALL WAITS, and it is not
// padding. The last step of a load — registering this module's credential with
// capability_module, without which every call it makes is refused "token not
// recognized" — happens on the CORE, after it reads the contract-query reply
// this image sent. The page cannot observe that step: there is no frame for it
// and no signal on this side of the bridge. So the wait is a settle, stated as
// one rather than hidden inside a poll interval, and it is generous because the
// thing it is waiting for takes microseconds once the reply has landed.
//
// It is also why the first ask RETRIES, and the RETRY is the part that makes
// this correct: a refusal here is a race, not an answer, and a wallet that
// showed "no accounts" because it asked half a second early would be wrong in
// the way that is hardest to notice. Because the retry carries the correctness,
// the settle only has to cover the common case, so it is short and the retries
// are close together — six over three seconds rather than one wait of two.
// A phone's first paint of the account list is the thing being spent here.
constexpr int kAdmissionSettleMs = 250;
// THE RETRY BUDGET FOR THAT RACE, shared by every first ask this variant makes.
// Not the account list's alone: measured on an iPad Air 13-inch simulator, the
// keystore AND railgun were both refused "token not recognized (re-exchange
// failed)" in the same turn, and only the ask that retried ever got an answer
// (logos-workspace#235). Bounded, because a module that really is absent must
// be REPORTED rather than polled for the life of the page.
constexpr int kAdmissionRetries = 6;
constexpr int kAdmissionRetryMs = 500;

// Every rust-first module on this wire answers a JSON TEXT, not a structure,
// so a reply is parsed before it is read.
QJsonObject replyOf(const logos::web::ModuleCallResult& res)
{
    return QJsonDocument::fromJson(res.value.toString().toUtf8()).object();
}

// `{ "ok": true, "result": … }` / `{ "ok": false, "error": … }` — eth_rpc's
// envelope, and the keystore's. Read here rather than in each caller so a
// refusal is never mistaken for a value.
QJsonValue resultOf(const QJsonObject& reply)
{
    if (!reply.value(QStringLiteral("ok")).toBool())
        return QJsonValue();
    return reply.value(QStringLiteral("result"));
}

QString errorOf(const QJsonObject& reply)
{
    return reply.value(QStringLiteral("error")).toString();
}

// WHAT THIS BACKEND IS DOING, ON THE PAGE'S CONSOLE.
//
// A `web` variant's backend is a wasm image inside a page inside a webview:
// there is no process to attach to, no log file it owns and no property a host
// can read while it is starting. The container forwards a page's console to the
// app's log, so that is where this says what it did — and the device is the only
// place several of these lines have ever been needed.
void announce(const QString& what)
{
    qInfo().noquote() << QStringLiteral("[wallet_ui web] %1").arg(what);
}

// Every call across the door carries JSON as TEXT, so a structure is rendered
// on the way out exactly as `replyOf` parses one on the way in. One name for
// both shapes: which of the two is being rendered is never the interesting part.
QString jsonText(const QJsonObject& obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString jsonText(const QJsonArray& arr)
{
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// WHY A WALLET NAMES ITSELF CUSTODIAN AT ALL. Creating an account is Tier D in
// the keystore: it belongs to the CUSTODIAN, whose built-in default is
// `evm_keystore_ui`. That module does not exist in this workspace and certainly
// does not run on a phone, so with the defaults in force the New-account dialog
// would answer "not authorized" for ever. Until a keystore UI ships, this wallet
// is the only surface a user can create an account from, and it says so out loud
// rather than by being silently admitted.
//
// ADDED TO THE SET, NOT PUT IN PLACE OF IT. `configure` is TOTAL — a role this
// document does not name is held by NOBODY — so naming only this module would
// revoke `evm_keystore_ui`'s custody and `evm_signer_ui`'s approval in the same
// call. A role is a set precisely so a second holder can be added, and that is
// all this adds. The desktop variant sends the same document; the two are
// separate images with no shared translation unit, so they must stay in step by
// hand.
//
// AND WHY THE APPROVER SET HAS TWO NAMES (logos-workspace#245). The same
// argument, one role along. `keystore_module` signs nothing without a human:
// this wallet asks through `request_approval`, and only a configured APPROVER
// may claim the request, read the keystore's own render lines and answer
// `approve(handle, bundle_id, password)`. The built-in approver is
// `evm_signer_ui` — the desktop Signer app, which is not in this workspace and
// does not run on a phone — so on a device the role was held by a module that
// could never answer, and every approval this wallet asked for parked at `sign`
// for ever. That is the whole of why the shield's `wrap` / `approve` / `shield`
// legs had never reached a chain.
//
// `evm_signer_cli` is the headless approver: the same role and the same gate —
// it re-claims the request, checks the bundle id against the one on screen and
// takes the vault password — driven by method calls rather than a window. It is
// a Bundled member of the mobile app image beside this wallet. Naming it here
// does not admit it: the keystore admits a CONFIGURED approver, and this is the
// document that configures one.
QString keystoreRoles()
{
    QJsonObject roles;
    roles.insert(QStringLiteral("approvers"),
                 QJsonArray{ QStringLiteral("evm_signer_ui"),
                             QStringLiteral("evm_signer_cli") });
    roles.insert(QStringLiteral("custodians"),
                 QJsonArray{ QStringLiteral("evm_keystore_ui"), QStringLiteral("wallet_ui") });
    return jsonText(roles);
}

// The two flags every startup line and every outbound call is announced with,
// always together and always in the same words: "the page has a channel" and
// "the core has admitted this module" fail for different reasons, and only the
// second is the one to act on.
QString doorState()
{
    return QStringLiteral("channel=%1 admitted=%2")
        .arg(logos::web::canCallModules() ? "yes" : "no")
        .arg(logos::web::hostAdmitted() ? "yes" : "no");
}

// WHAT A SLOT WITH A RETURN VALUE ANSWERS when the work it started is on the
// other side of the door: the ask was TAKEN, and the answer will land in a
// PROP. The counterpart of `refuse()`, which is the same envelope for a method
// this variant cannot serve at all.
QString accepted()
{
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("pending"), true);
    return jsonText(out);
}

// The other envelope: the ask was NOT taken, and this is why. Shared by
// `refuse()` — which says a module is missing — and by the checks a method
// makes on its own arguments, which say nothing about any module at all.
QString failed(const QString& why)
{
    QJsonObject out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), why);
    return jsonText(out);
}

// THE TWO THINGS THAT HAVE TO BE TRUE for a call to have worked: the door
// delivered it, and the module said yes. Read together because either one alone
// is a refusal, and a reply read as a value when `ok` was false is the mistake
// this exists to stop.
bool callSucceeded(const logos::web::ModuleCallResult& res, const QJsonObject& reply)
{
    return res.ok && reply.value(QStringLiteral("ok")).toBool();
}

// WHY `callSucceeded` SAID NO, in the module's words or the door's. The two are
// the same outcome to a caller but never the same reason: a call that never
// arrived carries the transport's error, one the module turned down carries the
// module's. Read here, once, so no caller has to remember which field to look
// in — and so every refusal on the status line names the thing that refused.
QString refusalReason(const logos::web::ModuleCallResult& res, const QJsonObject& reply)
{
    return res.ok ? errorOf(reply) : res.error;
}

// "railgun_module refused relayed_send: …" — the one sentence a refusal is said
// in, written once so every caller names the module it asked and the method it
// asked for, in the same order and with the same punctuation.
QString moduleRefused(const QString& module, const QString& method,
                      const logos::web::ModuleCallResult& res, const QJsonObject& reply)
{
    return QStringLiteral("%1 refused %2: %3").arg(module, method, refusalReason(res, reply));
}

// A ROUTE LAID OUT FRESH: every leg named, in the order it is walked, and all of
// them `pending`. The send's route and the shield's are the same shape — a
// named leg with a state each — so they are laid out and moved by the same two
// helpers rather than by two copies of them.
QJsonArray pendingRoute(std::initializer_list<QString> legs)
{
    QJsonArray route;
    for (const QString& leg : legs)
        route.append(QJsonObject{ { QStringLiteral("name"), leg },
                                  { QStringLiteral("state"), kStatePending } });
    return route;
}

// ...and moving one leg of it, along with the line that names what is RUNNING.
// `running` is only moved by a leg that STARTS: a leg that ends leaves it where
// it was, which is what lets a failed or cancelled route still say which leg it
// was on.
void markLeg(QJsonArray& route, QString& running, const QString& leg, const QString& state)
{
    for (int i = 0; i < route.size(); ++i) {
        QJsonObject entry = route.at(i).toObject();
        if (entry.value(QStringLiteral("name")).toString() != leg)
            continue;
        entry.insert(QStringLiteral("state"), state);
        route.replace(i, entry);
        break;
    }
    if (state == kStateRunning)
        running = leg;
}

// ...AND THE SAME, ON THE PAGE'S CONSOLE, for the keystore — whose callers
// differ only in what they say afterwards, so the line itself is written here.
QString announceKeystoreRefusal(const QString& method, const logos::web::ModuleCallResult& res,
                        const QJsonObject& reply)
{
    const QString why = refusalReason(res, reply);
    announce(QStringLiteral("keystore_module refused %1: %2").arg(method, why));
    return why;
}

} // namespace

WalletUiWebBackend::WalletUiWebBackend(QObject* parent)
    : WalletUiSimpleSource(parent)
{
    seedDefaultChains();
    publishChains();
    setAccountsJson(QStringLiteral("{\"accounts\":[]}"));
    setBalancesJson(QStringLiteral("{\"balances\":{\"chains\":[]}}"));
    setStatusText(QStringLiteral("Ready — balances through eth_rpc_module"));
    // Something true on the Private tab from the first paint: nothing is known
    // about the accumulator yet, and an empty bar that claimed 0 % would be a
    // guess. The first `sync_status` replaces this the moment the page is
    // admitted.
    publishPrivateSync(kStateIdle, QJsonObject{}, QStringLiteral("Not checked yet."));
    // ...and the same for the send: the route it would take, with nothing run.
    resetSendRoute();
    publishPrivateSend(kStateIdle, QStringLiteral("No private send has been started."));
    // ...and for the shield, which is the route in the other direction. Both
    // are published BEFORE anything can run, because the thing #235 is about is
    // a user who cannot tell a working app from a hung one: a page that names
    // its route from the first paint has already answered half of that.
    resetShieldRoute();
    publishPrivateShield(kStateIdle, QStringLiteral("No shield has been started."));

    // AUTOMATIC, AND ONLY AT THE EDGES. The view already picks the first
    // account as soon as one exists (WalletView.qml's `onCountChanged`), and it
    // writes it back through the READWRITE `selectedAccount`. Fetching that
    // account's balances here is what makes the two halves one flow: open the
    // app, see what you hold. A refresh is still one tap, for an account the
    // user picks or a chain list they change.
    connect(this, &WalletUiWebBackend::selectedAccountChanged, this,
            [this](const QString& address) {
                announce(QStringLiteral("the view picked %1").arg(address));
                if (!address.isEmpty())
                    refreshBalances(address);
            });

    startWhenReachable();
}

// THE DOOR IS NOT OPEN AT CONSTRUCTION, and "open" is not the edge to wait for.
// This image is up before the page has bound the container's bridge, so the
// first call cannot be made now. But the bridge resolves EARLY — while the
// container is still asking the page what it serves — and an outbound call
// made in that window is a synchronous native call on the container's own
// thread, inside its own admission round trip: the capability handshake runs
// for its whole budget against a module the core has not finished registering,
// the container's deadline passes underneath it, and the module is reported as
// "the page never published a module".
//
// MEASURED, on the iPad Air 13-inch simulator, which is how this comment came
// to exist: waiting on canCallModules() alone failed the load exactly that way.
// So the wait is hostAdmitted() — the core has sent this page the credential it
// minted for it, which is the last step of the load and the first moment
// anything here could be authorized anyway.
//
// A poll rather than a signal because the seam that would carry one is
// JavaScript's, on the other side of embind, and a 100 ms tick costs nothing
// against a page that takes seconds to come up. It gives up: a `web` variant
// loaded with no host (a page opened by hand, a container with no core) would
// otherwise poll for the life of the process, and a module that says what is
// wrong is worth more than one that keeps trying silently.
void WalletUiWebBackend::startWhenReachable()
{
    m_startup.setInterval(kStartupPollMs);
    connect(&m_startup, &QTimer::timeout, this, [this]() {
        if (logos::web::hostAdmitted()) {
            m_startup.stop();
            announce(QStringLiteral("admitted after %1 ms; asking %2 for accounts in %3 ms")
                         .arg(m_startupTicks * kStartupPollMs)
                         .arg(kKeystore)
                         .arg(kAdmissionSettleMs));
            QTimer::singleShot(kAdmissionSettleMs, this, [this]() {
                refreshAccounts();
                // AND HOW FAR BEHIND THE PRIVATE BALANCE IS, before anything is
                // offered. One `eth_blockNumber` and no chain walk — #235's
                // fourth question answered at the cheap end: the DISTANCE is
                // taken automatically, the WALK is not (see refreshPrivateSync).
                refreshPrivateSync();
            });
            return;
        }
        ++m_startupTicks;
        // ONCE A SECOND WHILE IT WAITS, and both flags (see doorState).
        if (m_startupTicks % (1000 / kStartupPollMs) == 0)
            announce(QStringLiteral("waiting for admission: %1 (%2 ms)")
                         .arg(doorState())
                         .arg(m_startupTicks * kStartupPollMs));
        if (m_startupTicks * kStartupPollMs > kStartupGiveUpMs) {
            m_startup.stop();
            announce(QStringLiteral("NO HOST: never admitted in %1 ms").arg(kStartupGiveUpMs));
            setStatusText(QStringLiteral(
                "No host: this page was never admitted by a Logos container"));
        }
    });
    m_startup.start();
}

// THE CHAIN LIST IS THIS VARIANT'S OWN. On the desktop it comes from
// `wallet_backend_module`, which owns the wallet's configuration; there is no
// such module on a phone, and eth_rpc stores an ENDPOINT per chain and nothing
// a view could render (no name, no native symbol). So the variant seeds a list
// and pushes each entry into eth_rpc as configuration — the two halves of what
// the coordinator used to hold, split where the modules actually are.
void WalletUiWebBackend::seedDefaultChains()
{
    const auto chain = [](int id, const char* name, const char* rpc, const char* sym) {
        QJsonObject c;
        c.insert(QStringLiteral("chainId"), id);
        c.insert(QStringLiteral("name"), QLatin1String(name));
        c.insert(QStringLiteral("rpcUrl"), QLatin1String(rpc));
        c.insert(QStringLiteral("nativeSymbol"), QLatin1String(sym));
        return c;
    };
    m_chains = QJsonArray{
        chain(1, "Ethereum", "https://ethereum-rpc.publicnode.com", "ETH"),
        chain(11155111, "Sepolia", "https://ethereum-sepolia-rpc.publicnode.com", "ETH"),
    };
}

// The seeded (or `setChains`-supplied) entry for a chain, empty when this
// variant does not know it.
QJsonObject WalletUiWebBackend::chainById(int chainId) const
{
    for (const QJsonValue& c : m_chains) {
        const QJsonObject o = c.toObject();
        if (o.value(QStringLiteral("chainId")).toInt() == chainId)
            return o;
    }
    return {};
}

void WalletUiWebBackend::publishChains()
{
    QJsonObject root;
    root.insert(QStringLiteral("chains"), m_chains);
    setChainsJson(jsonText(root));
}

// A METHOD THIS VARIANT HAS NOT WIRED UP, said in its own words.
//
// IT USED TO SAY "which has no mobile build", AND THAT IS NOW FALSE. Every
// module this file names has a mobile build: `eth_rpc_module`, `uniswap_module`
// and `token_list_module` are Bundled Bare members, `keystore_module` reaches a
// phone as a `web` variant, and `wallet_backend_module` has been a catalog
// member since #183 -- which is what made logos-workspace#250 read like a
// packaging bug. A refusal that misnames its reason sends a reader looking for a
// missing build that is not missing, and #147 measured what that costs one
// module over.
//
// So this says what is actually true: the route is not built here. Sends and fee
// estimation are the ones left -- a send parks a signing request with
// `keystore_module` and is polled to a broadcast, and none of that route exists
// in this variant yet. The module that would serve it is still NAMED, because
// that is the part a developer reading the screen needs.
QString WalletUiWebBackend::refuse(const QString& what, const QString& module)
{
    const QString message =
        QStringLiteral("%1 is %2's, and the `web` variant does not drive it yet")
            .arg(what, module);
    setStatusText(message);
    return failed(message);
}

// ...AND WHAT A COORDINATOR CALL THAT REALLY WAS REFUSED SAYS. Separate from
// `refuse()` above because the two are different facts: one is a route this
// image never built, the other is a module that answered, or a door that could
// not reach one. `moduleRefused` puts the module's own words on the line, which
// is how "the image does not carry the coordinator" and "the coordinator has no
// state for this device" stay apart on a screen.
void WalletUiWebBackend::coordinatorRefused(const QString& method,
                                            const logos::web::ModuleCallResult& res,
                                            const QJsonObject& reply)
{
    const QString said = moduleRefused(kWalletBackend, method, res, reply);
    announce(said);
    setStatusText(said);
}

// ── config ───────────────────────────────────────────────────────────────────

void WalletUiWebBackend::ensureChainConfig(int chainId, const QString& endpoint,
                                           std::function<void()> then)
{
    if (m_chainsConfigured.contains(chainId) || endpoint.isEmpty()) {
        if (then)
            then();
        return;
    }
    m_chainsConfigured.insert(chainId);

    QJsonObject cfg;
    cfg.insert(QStringLiteral("endpoint"), endpoint);
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("set_chain_config"),
        QJsonArray{ chainId, jsonText(cfg) },
        [this, chainId, then](const logos::web::ModuleCallResult& res) {
            // A REFUSAL IS NOT CACHED. set_chain_config answers a bare bool, so
            // a `false` — or a call that never reached the module — has to
            // clear the mark or every later balance on this chain would be
            // asked of an endpoint eth_rpc was never told about.
            if (!res.ok || !res.value.toBool()) {
                m_chainsConfigured.remove(chainId);
                const QString why = res.ok ? QStringLiteral("bad config") : res.error;
                // The same words on the console and in the view's status line:
                // a device run reads the first and a human reads the second.
                const QString refusal = QStringLiteral("eth_rpc_module refused chain %1: %2")
                                            .arg(chainId).arg(why);
                announce(refusal);
                setStatusText(refusal);
            } else {
                announce(QStringLiteral("eth_rpc_module configured chain %1").arg(chainId));
            }
            // ALWAYS, and always last: the caller's next step is a question
            // ABOUT this chain, and a refusal answered by eth_rpc in its own
            // words beats a balance that silently never went out.
            if (then)
                then();
        });
}

namespace {

// THE SETTINGS LINE FOR A DOCUMENT THE COORDINATOR ACCEPTED, which says WHAT
// was applied and not that something was: the settings tab has two fields, and
// a user who mistyped one needs to see which value the wallet is running with.
//
// `proxyRequired` is on both lines because it is the one that decides what
// happens when the proxy is unreachable: a cleared proxy that is still required
// is a wallet that will read nothing at all.
QString proxyApplied(const QString& url, bool required)
{
    if (url.isEmpty()) {
        return required ? QStringLiteral("Proxy cleared — but still required, so chain "
                                         "reads will fail closed")
                        : QStringLiteral("Proxy cleared");
    }
    return required ? QStringLiteral("Proxy applied: %1 (required)").arg(url)
                    : QStringLiteral("Proxy applied: %1 (optional)").arg(url);
}

} // namespace

// THE PROXY SETTINGS ARE THE COORDINATOR'S, and they are asked for now (#250).
//
// `wallet_backend_module` holds them and pushes them into `eth_rpc_module` for
// every configured chain, which is why this is one call and not a walk over
// `m_chains`: the fail-closed guarantee belongs to whoever owns the setting, and
// a page that configured each chain itself would be a second copy of it.
//
// IT ANSWERS A BARE BOOL, not the `{ok, …}` envelope most of this wire uses --
// the same shape as eth_rpc's `set_chain_config`, and read the same way. A
// `false` is a REFUSAL: the module could not parse the document or had no state
// to put it in, and reporting it as applied would leave a user believing their
// traffic was proxied.
//
// The SLOT answers "taken", not "applied": the reply lands on `proxyStatus`,
// because there is no value to return by the time this returns.
bool WalletUiWebBackend::setProxyConfig(QString proxyJson)
{
    if (proxyJson.trimmed().isEmpty()) {
        const QString why = QStringLiteral("Proxy config needs a document");
        setProxyStatus(why);
        setStatusText(why);
        return false;
    }

    announce(QStringLiteral("setProxyConfig: asking %1, %2").arg(kWalletBackend, doorState()));
    setProxyStatus(QStringLiteral("Applying…"));
    logos::web::callModuleAsync(
        kWalletBackend, QStringLiteral("set_proxy_config"), QJsonArray{ proxyJson },
        [this, proxyJson](const logos::web::ModuleCallResult& res) {
            if (!res.ok || !res.value.toBool()) {
                // The module's words when it has them, the door's when it does
                // not -- and never "no mobile build", which is what #250 found
                // on this line with the coordinator sitting in the image.
                const QString said = res.ok
                    ? QStringLiteral("%1 refused set_proxy_config: the document could "
                                     "not be applied").arg(kWalletBackend)
                    : moduleRefused(kWalletBackend, QStringLiteral("set_proxy_config"),
                                    res, QJsonObject{});
                announce(said);
                setProxyStatus(said);
                setStatusText(said);
                return;
            }
            // Read back from the document that was sent, because the module
            // answers a bare `true` and carries nothing to report.
            const QJsonObject asked =
                QJsonDocument::fromJson(proxyJson.toUtf8()).object();
            const QString applied =
                proxyApplied(asked.value(QStringLiteral("proxy")).toString(),
                             asked.value(QStringLiteral("proxyRequired")).toBool());
            announce(applied);
            setProxyStatus(applied);
            setStatusText(applied);
        });
    return true;
}

bool WalletUiWebBackend::setChains(QString chainsJson)
{
    // Either shape the view may send: the wrapped `{"chains":[…]}` this
    // backend publishes, or a bare list.
    const QJsonDocument doc = QJsonDocument::fromJson(chainsJson.toUtf8());
    const QJsonArray list = doc.isArray()
        ? doc.array()
        : doc.object().value(QStringLiteral("chains")).toArray();
    if (list.isEmpty()) {
        setStatusText(QStringLiteral("Set chains failed: no chains in the list"));
        return false;
    }
    m_chains = list;
    m_chainsConfigured.clear();
    publishChains();
    for (const QJsonValue& c : m_chains) {
        const QJsonObject o = c.toObject();
        ensureChainConfig(o.value(QStringLiteral("chainId")).toInt(),
                          o.value(QStringLiteral("rpcUrl")).toString());
    }
    setStatusText(QStringLiteral("Chains updated"));
    return true;
}

// ACCEPTED, NOT ANSWERED. Every `.rep` SLOT with a return value is synchronous
// on this side of the replica and the door is not, so what a caller gets back
// here is that the ask was taken — the answer lands in a PROP. The desktop
// backend can return the real thing; this one structurally cannot, and saying
// so is better than blocking an event loop that would never be pumped.
QString WalletUiWebBackend::testEndpoint(int chainId)
{
    setStatusText(QStringLiteral("Testing chain %1…").arg(chainId));
    // CONFIGURE, THEN ASK — the same two steps a balance takes, for the same
    // reason: the endpoint has to be in eth_rpc before it is asked to reach it.
    ensureChainConfig(chainId,
                      chainById(chainId).value(QStringLiteral("rpcUrl")).toString(),
                      [this, chainId]() { verifyChain(chainId); });
    return accepted();
}

// The second half of testEndpoint, a named function rather than a lambda inside
// a lambda — so the configure/ask pair reads the same here as it does in
// refreshBalances, where the continuation is `fetchBalance`.
void WalletUiWebBackend::verifyChain(int chainId)
{
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("verify_chain_id"), QJsonArray{ chainId },
        [this, chainId](const logos::web::ModuleCallResult& res) {
            if (!res.ok) {
                setStatusText(QStringLiteral("chain %1: %2").arg(chainId).arg(res.error));
                return;
            }
            const QJsonObject reply = replyOf(res);
            const QJsonValue got = resultOf(reply);
            setStatusText(got.isNull()
                              ? QStringLiteral("chain %1: %2").arg(chainId).arg(errorOf(reply))
                              : QStringLiteral("chain %1 answered").arg(chainId));
        });
}

// ── accounts: keystore_module ────────────────────────────────────────────────

void WalletUiWebBackend::refreshAccounts()
{
    announce(QStringLiteral("refreshAccounts: asking %1, %2").arg(kKeystore, doorState()));
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("list_accounts"), QJsonArray{},
        [this](const logos::web::ModuleCallResult& res) {
            if (!res.ok) {
                announce(QStringLiteral("keystore_module refused list_accounts: %1")
                             .arg(res.error));
                setStatusText(QStringLiteral("keystore_module: %1").arg(res.error));
                // See kAdmissionSettleMs: a refusal this early is a race with
                // the core's own load path, not an answer.
                if (m_accountRetries++ < kAdmissionRetries)
                    QTimer::singleShot(kAdmissionRetryMs, this,
                                       [this]() { refreshAccounts(); });
                return;
            }
            m_accountRetries = 0;
            const QJsonObject reply = replyOf(res);   // `{ok, accounts:[…]}`
            const QJsonArray accounts = reply.value(QStringLiteral("accounts")).toArray();
            QJsonObject out;
            out.insert(QStringLiteral("accounts"), accounts);
            setAccountsJson(jsonText(out));

            // AND PICK ONE, HERE, rather than leaving it to the view.
            //
            // "Open the app, see what you hold" is the whole of this variant's
            // job, and it needs an account selected before a balance can be
            // asked for. The view has an auto-select — WalletView.qml's
            // `onCountChanged` — and on the device it does not fire for this
            // path: measured on an iPad Air 13-inch simulator, the accounts
            // reached the replica and `selectedAccount` stayed empty, so
            // nothing ever asked eth_rpc anything. A backend that depends on a
            // view to choose cannot work headless either, and this one is asked
            // things over the door with no view attached. Only when nothing is
            // selected, so a user's own pick is never overridden.
            if (selectedAccount().isEmpty() && !accounts.isEmpty())
                setSelectedAccount(accounts.first().toString());

            announce(QStringLiteral("accounts: %1 from keystore_module%2")
                         .arg(accounts.size())
                         .arg(accounts.isEmpty()
                                  ? QString()
                                  : QStringLiteral(", first %1")
                                        .arg(accounts.first().toString())));
        });
}

QString WalletUiWebBackend::createAccount(QString passphrase, QString label)
{
    Q_UNUSED(label)
    setStatusText(QStringLiteral("Creating account…"));
    // TAKE THE ROLE, THEN MUTATE — and CHAINED, because two calls issued in one
    // turn are answered in whatever order the container finishes them (see
    // ensureChainConfig). Asking for the account before the role is in force is
    // the same race, and it fails as "not authorized" rather than as a timeout.
    claimCustody([this, passphrase]() {
        QJsonObject params;
        params.insert(QStringLiteral("password"), passphrase);
        // THE ACKNOWLEDGEMENT IS THE METHOD'S POINT, not a formality: an
        // unrelated account is a key no recovery phrase covers, and the keystore
        // refuses to mint one unless the caller has said so. This variant says
        // so on the user's behalf because the New-account dialog IS that choice
        // — there is no seed phrase anywhere in this wallet to derive from.
        params.insert(QStringLiteral("acknowledgeUnrecoverable"), true);
        logos::web::callModuleAsync(
            kKeystore, QStringLiteral("create_unrelated_account"),
            QJsonArray{ jsonText(params) },
            [this](const logos::web::ModuleCallResult& res) {
                const QJsonObject reply = replyOf(res);
                if (keystoreRefused(QStringLiteral("create_unrelated_account"), res, reply))
                    return;
                announce(QStringLiteral("created %1")
                             .arg(reply.value(QStringLiteral("address")).toString()));
                setStatusText(QStringLiteral("Account created"));
                refreshAccounts();
            });
    });
    return accepted();
}

bool WalletUiWebBackend::keystoreRefused(const QString& method,
                                         const logos::web::ModuleCallResult& res,
                                         const QJsonObject& reply)
{
    if (callSucceeded(res, reply))
        return false;
    // The same words go to the console and to the view's status line: a device
    // run reads the first and a human reads the second.
    const QString why = announceKeystoreRefusal(method, res, reply);
    setStatusText(QStringLiteral("keystore_module: %1").arg(why));
    return true;
}

void WalletUiWebBackend::claimCustody(std::function<void()> then)
{
    // ASK WHO THE KEYSTORE THINKS WE ARE, FIRST. Tier D admits a plainly NAMED
    // module and nothing else: a host anchor, a derived credential and an
    // operator token are all refused, and all three refuse with the same words
    // as a wrong name. On a phone there is no other way to ask — no process to
    // attach to, no CLI that shares this credential — so a run that is refused
    // says which of the two it was instead of leaving them indistinguishable.
    // UNGATED and side-effect-free, by contract, so asking costs nothing but the
    // round trip.
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("caller_identity"), QJsonArray{},
        [this, then](const logos::web::ModuleCallResult& res) {
            const QJsonObject who = replyOf(res);
            announce(QStringLiteral("keystore_module sees this caller as %1 \"%2\"; "
                                    "custodians %3")
                         .arg(who.value(QStringLiteral("kind")).toString(),
                              who.value(QStringLiteral("identity")).toString(),
                              jsonText(who.value(QStringLiteral("custodians")).toArray())));
            takeCustodianRole(then);
        });
}

void WalletUiWebBackend::takeCustodianRole(std::function<void()> then)
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("configure"), QJsonArray{ keystoreRoles() },
        [this, then](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (keystoreRefused(QStringLiteral("configure"), res, reply))
                return;
            announce(QStringLiteral("custodians are now %1")
                         .arg(jsonText(reply.value(QStringLiteral("custodians")).toArray())));
            then();
        });
}

void WalletUiWebBackend::nameAnApprover(std::function<void()> then,
                                        std::function<void(const QString&)> refused)
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("configure"), QJsonArray{ keystoreRoles() },
        [this, then, refused](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                refused(moduleRefused(kKeystore, QStringLiteral("configure"), res, reply));
                return;
            }
            announce(QStringLiteral("approvers are now %1")
                         .arg(jsonText(reply.value(QStringLiteral("approvers")).toArray())));
            then();
        });
}

// IMPORTING A SEED PHRASE IS THE KEYSTORE'S OWN METHOD, and this variant asks
// for it exactly as it asks for a new account (#147).
//
// THE DESKTOP GOES THROUGH THE COORDINATOR AND THIS DOES NOT, which is the only
// real difference between the two. `wallet_backend_module.import_mnemonic`
// forwards the phrase document to `keystore_module.import_mnemonic` unchanged
// and then keeps `address -> label` in a labels.json of its own; there is no
// coordinator here and no such file, so the phrase goes straight to the keystore
// and the LABEL goes where this variant can actually put it — the keystore's own
// label store, which `get_labels` reads back.
//
// TIER D, so the same claim-then-mutate chain `createAccount` runs: importing a
// key belongs to the custodian, and the role has to be in force before the
// mutation is asked for.
QString WalletUiWebBackend::importMnemonic(QString phraseJson, QString label)
{
    // The document the view sends IS the keystore's params document —
    // `{ phrase, accountIndex, password }` from the Advanced tab, and every
    // optional field `import_mnemonic` understands (`passphrase`, `storage`,
    // `bip44Account`, `groupLabel`, …) if a caller sends one. Forwarded whole
    // rather than rebuilt field by field, so a keystore that grows a parameter
    // does not need this file edited to pass it.
    const QJsonObject params = QJsonDocument::fromJson(phraseJson.toUtf8()).object();

    // ASKED AND ANSWERED HERE: a missing phrase is this call's own defect, not a
    // module's absence and not the keystore's refusal to look at. Saying so
    // without a round trip is both faster and truer than letting the keystore
    // answer "invalid mnemonic" for an empty string.
    if (params.value(QStringLiteral("phrase")).toString().trimmed().isEmpty()) {
        const QString why = QStringLiteral("Import needs a seed phrase");
        announce(why);
        setStatusText(why);
        return failed(why);
    }

    setStatusText(QStringLiteral("Importing account…"));
    // The password is lifted out here rather than read off `params` again in the
    // reply: the label call needs it, and the phrase does not have to be carried
    // into a second lambda to get it there.
    const QString password = params.value(QStringLiteral("password")).toString();
    claimCustody([this, params, label, password]() {
        logos::web::callModuleAsync(
            kKeystore, QStringLiteral("import_mnemonic"), QJsonArray{ jsonText(params) },
            [this, label, password](const logos::web::ModuleCallResult& res) {
                const QJsonObject reply = replyOf(res);
                if (keystoreRefused(QStringLiteral("import_mnemonic"), res, reply))
                    return;
                const QString address = reply.value(QStringLiteral("address")).toString();
                announce(QStringLiteral("imported %1 at %2")
                             .arg(address, reply.value(QStringLiteral("path")).toString()));
                setStatusText(QStringLiteral("Account imported"));
                labelAccount(address, label, password);
            });
    });
    return accepted();
}

// The label the user typed beside the phrase, put where this variant can keep
// it, and then the account list — CHAINED, for the same reason every other pair
// of calls here is: two issued in one turn are answered in whatever order the
// container finishes them.
//
// A REFUSED LABEL IS NOT A FAILED IMPORT. The key is in the keystore either way
// and losing sight of it because a name did not stick would be the worse
// outcome, so this reports the refusal beside "Account imported" rather than in
// place of it, and goes on to re-read the list.
void WalletUiWebBackend::labelAccount(const QString& address, const QString& label,
                                      const QString& password)
{
    if (label.isEmpty() || address.isEmpty()) {
        refreshAccounts();
        return;
    }
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("set_label"), QJsonArray{ address, label, password },
        [this, address, label](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (callSucceeded(res, reply)) {
                announce(QStringLiteral("labelled %1 \"%2\"").arg(address, label));
            } else {
                // NOT `keystoreRefused`: that one replaces the status line with
                // the keystore's reason, which here would unsay "Account
                // imported" for a key that is in the keystore all the same.
                const QString why =
                    announceKeystoreRefusal(QStringLiteral("set_label"), res, reply);
                setStatusText(QStringLiteral("Account imported — label not set: %1").arg(why));
            }
            refreshAccounts();
        });
}

QString WalletUiWebBackend::sendStatus(QString requestId)
{
    Q_UNUSED(requestId)
    // Nothing this variant does raises a signing request, so nothing can be
    // pending. Naming the module keeps the refusal diagnosable rather than
    // letting a poll spin against a state that will never change.
    return refuse(QStringLiteral("Signing requests"),
                  QStringLiteral("wallet_backend_module"));
}

// ── balances: the Bundled eth_rpc_module ─────────────────────────────────────
//
// THE CRITERION, in one fan-out. One `eth_getBalance` per configured chain,
// each an independent call over the door; the replies land in whatever order
// the container answers and the aggregate is published when the last one is
// in. A counter rather than a join: there is one event loop, so the decrement
// and the publish cannot interleave with each other.
void WalletUiWebBackend::refreshBalances(QString address)
{
    if (address.isEmpty()) {
        setStatusText(QStringLiteral("Pick an account first"));
        return;
    }
    const quint64 epoch = ++m_balanceEpoch;
    m_balances = QJsonObject();
    // COUNTED IN FULL BEFORE ANY CALL GOES OUT, because the chains no longer
    // start their calls together: one already configured fetches from inside
    // this loop, one not yet configured fetches from a later turn. Counting a
    // chain in as its fetch is issued would let the first chain's reply find a
    // counter of 1, drop it to 0 and publish an aggregate still missing every
    // chain whose configuration had not answered yet.
    m_balancePending = m_chains.size();
    setStatusText(QStringLiteral("Refreshing balances…"));
    announce(QStringLiteral("refreshBalances(%1): %2 chain(s), %3")
                 .arg(address)
                 .arg(m_chains.size())
                 .arg(doorState()));

    for (const QJsonValue& c : m_chains) {
        const QJsonObject o = c.toObject();
        const int chainId = o.value(QStringLiteral("chainId")).toInt();
        const QString symbol = o.value(QStringLiteral("nativeSymbol")).toString();
        // CONFIGURE, THEN ASK — see ensureChainConfig's note. Issuing both in
        // one turn is what made every balance read
        // "unavailable (no configuration for chain 1)" on the device.
        ensureChainConfig(
            chainId, o.value(QStringLiteral("rpcUrl")).toString(),
            [this, epoch, address, chainId, symbol]() {
                // A fan-out that has been superseded does not get to spend a
                // call: the counter it would decrement belongs to the new one.
                if (epoch != m_balanceEpoch)
                    return;
                fetchBalance(address, chainId, symbol);
            });
    }
    if (m_balancePending == 0)
        publishBalances();
}

void WalletUiWebBackend::fetchBalance(const QString& address, int chainId,
                                      const QString& symbol)
{
    const quint64 epoch = m_balanceEpoch;
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("get_balance"), QJsonArray{ chainId, address },
        [this, epoch, chainId, symbol](const logos::web::ModuleCallResult& res) {
            // A SECOND ASK SUPERSEDES THE FIRST. A reply belonging to a
            // fan-out that has been replaced is dropped rather than merged,
            // or the aggregate would carry two accounts' balances under one
            // name — or publish itself half-full. See m_balanceEpoch.
            if (epoch != m_balanceEpoch)
                return;

            announce(QStringLiteral("get_balance(%1) -> ok=%2 %3")
                         .arg(chainId)
                         .arg(res.ok ? "yes" : "no")
                         .arg(res.ok ? res.value.toString() : res.error));

            QJsonObject entry;
            entry.insert(QStringLiteral("chainId"), chainId);
            if (!res.ok) {
                entry.insert(QStringLiteral("native"),
                             QStringLiteral("unavailable (%1)").arg(res.error));
            } else {
                const QJsonObject reply = replyOf(res);
                const QJsonValue wei = resultOf(reply);
                entry.insert(QStringLiteral("native"),
                             wei.isString()
                                 ? QStringLiteral("%1 %2").arg(wei.toString(), symbol)
                                 : QStringLiteral("unavailable (%1)").arg(errorOf(reply)));
                // KEPT AS THE NUMBER, not re-parsed out of the line above: the
                // Market tab multiplies it by a price, and "<wei> ETH" is a
                // string built for a human. A chain that did not answer leaves
                // the last known value alone rather than storing a zero, which
                // would read as "you hold nothing" instead of "we do not know".
                if (wei.isString())
                    m_nativeWei.insert(chainId, wei.toString());
            }
            entry.insert(QStringLiteral("tokens"), QJsonArray{});
            m_balances.insert(QString::number(chainId), entry);

            if (--m_balancePending <= 0)
                publishBalances();
        });
}

void WalletUiWebBackend::publishBalances()
{
    // Back into chain order, which is the order the view renders: the replies
    // arrive in whatever order the chains answered and a list that reshuffles
    // itself between refreshes is a UI bug with no error anywhere.
    QJsonArray chains;
    for (const QJsonValue& c : m_chains) {
        const QString key =
            QString::number(c.toObject().value(QStringLiteral("chainId")).toInt());
        if (m_balances.contains(key))
            chains.append(m_balances.value(key));
    }
    QJsonObject inner;
    inner.insert(QStringLiteral("chains"), chains);
    QJsonObject root;
    root.insert(QStringLiteral("balances"), inner);
    setBalancesJson(jsonText(root));
    setStatusText(QStringLiteral("Balances updated"));
    announce(QStringLiteral("published balances: %1").arg(jsonText(inner)));
}

// ── tokens: the Bundled token_list_module (#148) ─────────────────────────────
//
// THE THIRD MODULE THIS VARIANT REACHES BY NAME, and the SIMPLEST of the three:
// one call, no configure first. An unconfigured token_list already serves the
// Uniswap default list compiled into its binary, so a user's first tap on this
// tab reads 1709 shipped rows and issues NO NETWORK I/O — which is the whole
// reason the tab can be answered from a phone at all. Fetching the live lists is
// `refresh_now`, it goes through that module's fail-closed proxy, and nothing
// here schedules it: a page must not decide when a device reaches the network.
//
// WHY NOT ensureChainConfig FIRST, as a balance and a price both do. Those two
// end in an RPC round trip that eth_rpc has to have an endpoint for. A token
// list is metadata: token_list holds it, keyed by chain id, and asks nobody.
void WalletUiWebBackend::loadTokens(int chainId)
{
    // One read in flight, superseded by the next — the same epoch shape the two
    // fan-outs above use, and needed here for the same reason even though this
    // is a single call: the view has one Tokens tab, so a reply for the chain
    // the user has just navigated away from must not repaint it.
    const quint64 epoch = ++m_tokensEpoch;
    setStatusText(QStringLiteral("Loading tokens…"));
    announce(QStringLiteral("loadTokens(%1): %2").arg(chainId).arg(doorState()));

    logos::web::callModuleAsync(
        kTokenList, QStringLiteral("get_tokens"), QJsonArray{ chainId },
        [this, epoch, chainId](const logos::web::ModuleCallResult& res) {
            if (epoch != m_tokensEpoch)
                return;

            announce(QStringLiteral("get_tokens(%1) -> ok=%2 %3")
                         .arg(chainId)
                         .arg(res.ok ? "yes" : "no")
                         .arg(res.ok ? QStringLiteral("%1 byte(s)").arg(res.value.toString().size())
                                     : res.error));

            const QJsonObject reply = replyOf(res);
            QJsonArray tokens;
            QString failure;
            if (callSucceeded(res, reply))
                tokens = reply.value(QStringLiteral("tokens")).toArray();
            else
                failure = refusalReason(res, reply);

            // PUBLISHED EITHER WAY, and empty on a failure. The tab shows one
            // chain at a time, so leaving the previous chain's rows up under a
            // chain that did not answer would be wrong with nothing to see.
            QJsonObject root;
            root.insert(QStringLiteral("tokens"), tokens);
            setTokensJson(jsonText(root));

            if (failure.isEmpty())
                setStatusText(QStringLiteral("%1 token(s) on chain %2")
                                  .arg(tokens.size())
                                  .arg(chainId));
            else
                setStatusText(QStringLiteral("token_list_module refused chain %1: %2")
                                  .arg(chainId)
                                  .arg(failure));
        });
}

// A CUSTOM TOKEN IS A MUTATION, and the `.rep` contract makes it a synchronous
// bool while the only door out of this image is asynchronous. So `true` here
// means THE ASK WAS TAKEN, not that it was stored — the outcome arrives on the
// status line, which is the same channel the view reads for every other result
// and the only one the QML dialog actually looks at.
//
// What IS answered here is what this image can see for itself: a document with
// no address or no chain is refused without a round trip, because
// `add_custom_token` answers a bare bool and a `false` from the module would be
// indistinguishable from a refusal for any other reason.
bool WalletUiWebBackend::addCustomToken(QString tokenJson)
{
    const QJsonObject token = QJsonDocument::fromJson(tokenJson.toUtf8()).object();
    const int chainId = token.value(QStringLiteral("chainId")).toInt();
    if (token.value(QStringLiteral("address")).toString().isEmpty() || chainId <= 0) {
        setStatusText(QStringLiteral("A custom token needs a chain and an address"));
        return false;
    }

    logos::web::callModuleAsync(
        kTokenList, QStringLiteral("add_custom_token"), QJsonArray{ jsonText(token) },
        [this, chainId](const logos::web::ModuleCallResult& res) {
            // A BARE BOOL, not the `{ok, …}` envelope the readers above parse:
            // `add_custom_token` answers the value itself, so `stored` is the
            // whole of what came back and a door failure is the only other
            // outcome there is.
            const bool stored = res.ok && res.value.toBool();
            QString outcome = res.error;
            if (res.ok)
                outcome = stored ? QStringLiteral("stored") : QStringLiteral("refused");
            announce(QStringLiteral("add_custom_token(chain %1) -> %2").arg(chainId).arg(outcome));

            if (!res.ok) {
                setStatusText(QStringLiteral("token_list_module never answered: %1").arg(res.error));
                return;
            }
            if (!stored) {
                setStatusText(QStringLiteral("token_list_module did not store the token"));
                return;
            }
            setStatusText(QStringLiteral("Token added"));
            // RE-READ THE CHAIN THE TOKEN WAS ADDED ON, not whichever the tab
            // last showed: the row the user has just typed is the one thing
            // they are looking for, and token_list merges it into that chain's
            // list under `source: "custom"`.
            loadTokens(chainId);
        });
    return true;
}

// ── market: the Bundled uniswap_module (#148) ───────────────────────────────
//
// THE SECOND MODULE THIS VARIANT REACHES BY NAME, and the same two-step shape as
// a balance: configure the chain on eth_rpc, then ask. The ask goes to UNISWAP,
// which issues its own Multicall3 `eth_call` back through eth_rpc — so the
// endpoint has to be in eth_rpc before the price is asked for, exactly as it
// does before a balance, and for the reason ensureChainConfig's note gives.
//
// WHAT IT PRICES, and why it still passes an empty token list now that
// `token_list_module` IS on the phone (#148). What that module publishes is a
// CATALOGUE — 1709 shipped rows, 401 of them on mainnet — not a watch list, and
// `get_prices` is a Multicall3 batch: handing it every row would spend a chain's
// RPC budget pricing tokens nobody holds. `{"tokens":[]}` is not an empty
// question either: `get_prices` always reports the chain's native asset,
// anchored on the chain's stablecoins through the pools it derives offline, so
// the Market tab shows a real ETH price on every chain uniswap has a deployment
// for. Pricing a user's OWN tokens wants the set they have chosen to watch,
// which nothing on a phone keeps yet — the coordinator did, and it has no mobile
// build. Left as it is rather than approximated from the catalogue.
//
// `address` IS UNUSED, and that is not an oversight. A price is a property of a
// chain, not of an account; the account only enters when a price is multiplied
// by a holding, which is `m_nativeWei` below. The parameter stays because the
// `.rep` contract and the desktop backend — where the coordinator prices the
// account's WATCHED tokens — both have it.
void WalletUiWebBackend::refreshMarket(QString address)
{
    const quint64 epoch = ++m_marketEpoch;
    m_market = QJsonObject();
    // COUNTED IN FULL BEFORE ANY CALL GOES OUT, for the reason refreshBalances
    // states: a chain already configured asks from inside this loop and one not
    // yet configured asks from a later turn, so counting as each ask is issued
    // would let the first reply find a counter of 1 and publish an aggregate
    // still missing every other chain.
    m_marketPending = m_chains.size();
    setStatusText(QStringLiteral("Loading market…"));
    announce(QStringLiteral("refreshMarket(%1): %2 chain(s), %3")
                 .arg(address)
                 .arg(m_chains.size())
                 .arg(doorState()));

    for (const QJsonValue& c : m_chains) {
        const QJsonObject o = c.toObject();
        const int chainId = o.value(QStringLiteral("chainId")).toInt();
        const QString symbol = o.value(QStringLiteral("nativeSymbol")).toString();
        ensureChainConfig(
            chainId, o.value(QStringLiteral("rpcUrl")).toString(),
            [this, epoch, chainId, symbol]() {
                if (epoch != m_marketEpoch)
                    return;
                fetchPrices(chainId, symbol);
            });
    }
    if (m_marketPending == 0)
        publishMarket();
}

void WalletUiWebBackend::fetchPrices(int chainId, const QString& symbol)
{
    const quint64 epoch = m_marketEpoch;
    logos::web::callModuleAsync(
        kUniswap, QStringLiteral("get_prices"),
        QJsonArray{ chainId, QStringLiteral("{\"tokens\":[]}") },
        [this, epoch, chainId, symbol](const logos::web::ModuleCallResult& res) {
            if (epoch != m_marketEpoch)
                return;

            announce(QStringLiteral("get_prices(%1) -> ok=%2 %3")
                         .arg(chainId)
                         .arg(res.ok ? "yes" : "no")
                         .arg(res.ok ? res.value.toString() : res.error));

            // A refusal is carried in uniswap's own words, which on a chain it
            // has no deployment for are "no uniswap config for chain <id>" — a
            // far more useful line than an empty list.
            const QJsonObject reply = replyOf(res);
            QJsonArray items;
            QString failure;
            if (callSucceeded(res, reply))
                items = priceItems(reply.value(QStringLiteral("prices")).toArray(),
                                   chainId, symbol);
            else
                failure = refusalReason(res, reply);

            QJsonObject entry;
            entry.insert(QStringLiteral("chainId"), chainId);
            entry.insert(QStringLiteral("items"), items);
            if (!failure.isEmpty())
                entry.insert(QStringLiteral("error"), failure);
            m_market.insert(QString::number(chainId), entry);

            if (--m_marketPending <= 0)
                publishMarket();
        });
}

QJsonArray WalletUiWebBackend::priceItems(const QJsonArray& prices, int chainId,
                                          const QString& symbol) const
{
    QJsonArray items;
    for (const QJsonValue& p : prices) {
        const QJsonObject price = p.toObject();
        const QString address = price.value(QStringLiteral("address")).toString();
        // Only the native asset is asked for today, so only the native asset is
        // labelled — with the chain's own symbol where the chain list gave one.
        // A token entry, when token_list crosses, carries its own address here.
        const QString label =
            (address == kNativeAsset && !symbol.isEmpty()) ? symbol : address;

        QJsonObject item;
        item.insert(QStringLiteral("symbol"), label);
        item.insert(QStringLiteral("usd"), price.value(QStringLiteral("usd")));
        const QJsonValue value = nativeValueUsd(chainId, price);
        if (!value.isNull())
            item.insert(QStringLiteral("valueUsd"), value);
        items.append(item);
    }
    return items;
}

QJsonValue WalletUiWebBackend::nativeValueUsd(int chainId, const QJsonObject& price) const
{
    if (price.value(QStringLiteral("address")).toString() != kNativeAsset)
        return {};
    const QJsonValue usd = price.value(QStringLiteral("usd"));
    if (!usd.isDouble())
        return {};
    const auto it = m_nativeWei.constFind(chainId);
    if (it == m_nativeWei.constEnd())
        return {};

    // WEI IS 256-BIT AND A DOUBLE IS NOT, so the scale comes off before the
    // conversion: `toDouble` on the decimal string is exact to ~15 digits, which
    // for a dollar figure shown to two decimal places is more precision than the
    // price it is multiplied by has. A value that does not parse (eth_rpc
    // answering something unexpected) is no value rather than zero.
    bool ok = false;
    const double wei = it.value().toDouble(&ok);
    if (!ok)
        return {};
    return wei / 1e18 * usd.toDouble();
}

void WalletUiWebBackend::publishMarket()
{
    // Chain order, not reply order — the same rule publishBalances states.
    QJsonArray chains;
    QStringList refusals;
    for (const QJsonValue& c : m_chains) {
        const int chainId = c.toObject().value(QStringLiteral("chainId")).toInt();
        const QString key = QString::number(chainId);
        if (!m_market.contains(key))
            continue;
        const QJsonObject entry = m_market.value(key).toObject();
        chains.append(entry);
        const QString why = entry.value(QStringLiteral("error")).toString();
        if (!why.isEmpty())
            refusals.append(QStringLiteral("chain %1: %2").arg(chainId).arg(why));
    }
    QJsonObject root;
    root.insert(QStringLiteral("chains"), chains);
    setMarketJson(jsonText(root));
    // A PARTIAL ANSWER SAYS SO. Sepolia has no Uniswap deployment in the
    // module's defaults, so a two-chain wallet legitimately prices one chain and
    // not the other; reporting "Market updated" for that would hide the only
    // line that explains the blank half.
    setStatusText(refusals.isEmpty()
                      ? QStringLiteral("Market updated")
                      : QStringLiteral("Market updated — %1").arg(refusals.join(QStringLiteral("; "))));
    announce(QStringLiteral("published market: %1").arg(jsonText(chains)));
}

QString WalletUiWebBackend::estimateFee(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Fee estimation"),
                  QStringLiteral("wallet_backend_module"));
}

QString WalletUiWebBackend::sendNative(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Sending"), QStringLiteral("wallet_backend_module"));
}

QString WalletUiWebBackend::sendErc20(QString sendJson)
{
    Q_UNUSED(sendJson)
    return refuse(QStringLiteral("Sending"), QStringLiteral("wallet_backend_module"));
}

// HISTORY IS THE COORDINATOR'S, and it is asked for now (#250).
//
// `wallet_backend_module` keeps a local record of everything this wallet
// broadcast and the receipts it has since read; there is nothing on the chain to
// reconstruct it from and no other module holds it. On the operator's iPad this
// method refused without asking, naming a module that was in the image -- so the
// tab reported "History needs wallet_backend_module" with the module unloaded
// three feet away.
//
// The reply IS the tab's document: `{ok, history: […]}` is what the view parses
// (`parseField(historyJson, "history", [])`), so a successful answer is
// published whole rather than rebuilt here.
void WalletUiWebBackend::refreshHistory(QString address)
{
    if (address.trimmed().isEmpty()) {
        setStatusText(QStringLiteral("History needs an account"));
        setHistoryJson(kEmptyHistory);
        return;
    }

    announce(QStringLiteral("refreshHistory: asking %1 for %2, %3")
                 .arg(kWalletBackend, address, doorState()));
    logos::web::callModuleAsync(
        kWalletBackend, QStringLiteral("get_history"), QJsonArray{ address },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                coordinatorRefused(QStringLiteral("get_history"), res, reply);
                // EMPTIED, NOT LEFT. The rows on screen belong to whatever was
                // asked for last, and leaving them under a refusal is a user
                // reading another account's transactions as this one's.
                setHistoryJson(kEmptyHistory);
                return;
            }
            setHistoryJson(res.value.toString());
            const int rows = reply.value(QStringLiteral("history")).toArray().size();
            setStatusText(rows == 0 ? QStringLiteral("No transactions yet")
                                    : QStringLiteral("History updated: %1 transaction(s)")
                                          .arg(rows));
        });
}

// ── the private sync: railgun_module, one window at a time ───────────────────
//
// WHAT IS BEING MITIGATED. A RAILGUN private send measured ~154 s on an iPad
// simulator and 239 s on a physical iPad Air 4, and ~92 % of that is the
// accumulator sync: witness + Groth16 prove + verify together are 3.7 s
// (logos-workspace#235, #213). `railgun_module.sync()` does that sync in ONE
// call which reports nothing, cannot be interrupted, and outlasts any caller's
// timeout. `sync_step` / `sync_status` / `sync_cancel` are the same work in
// bounded windows, and this section is the wallet asking for them.
//
// WHY railgun_module IS AN OPTIONAL DEPENDENCY AND NOT A REQUIRED ONE. The core
// resolves a module's REQUIRED dependencies before loading it and REFUSES a
// module whose list it cannot satisfy. `railgun_module` is a Bundled member an
// image carries only when it was asked for (`--bundle railgun_module`), so
// requiring it would mean a build without it could not load the wallet at
// all — trading every wallet build for a tab.
//
// IT WAS UNDECLARED UNTIL #250, AND THAT WAS THE OTHER HALF OF THE SAME BUG.
// "Discovered rather than declared" reads well and is not what the core does
// with a name it has never been given: an undeclared module is never brought up,
// so a page that asks for it is talking to something the loader left registered
// and idle. `web.optional_dependencies` (metadata.json) is the shape that says
// what was meant all along — bring it up if this image has it, and do not refuse
// the wallet if it does not — and it is what `wallet_backend_module` is declared
// under for exactly the same reason.
//
// A REFUSAL IS STILL PUBLISHED, because being loaded is not the same as being
// usable: `railgun_module` holds no engine until something calls `init` /
// `init_from_seed`, and NOTHING IN THIS WALLET DOES. Those keys are derived from
// a deterministic EOA signature the keystore will only produce behind a human
// approval in the Signer app, so bringing the private wallet up is its own route
// and is not built here yet. Until it is, this surface says so in the module's
// own words rather than showing a blank tab — see `privateSyncUnavailable`.
//
// ONE WINDOW IN FLIGHT AT A TIME, always. The next `sync_step` is issued from
// inside the last one's reply, which is the rule every chained call in this
// file follows (logos_web_module_call.h: "a backend that needs a sequence
// chains it in the callbacks"), and here it also carries the cancel: with never
// more than one window outstanding, "stop" is simply "do not ask for another".
namespace {

// The module that owns the accumulator. An OPTIONAL dependency of this variant
// (metadata.json: `web.optional_dependencies`) — see the section header.
const QString kRailgun = QStringLiteral("railgun_module");

// HOW BIG A WINDOW A VIEW WANTS, which is not the module's own default. A
// `sync_step` with no budget runs for 20 000 ms, so a progress bar would move
// three times a minute; 5 s is a window a user reads as motion and is still
// long enough that the per-call overhead is noise against it. The block count
// is the module's own default — it is the subsquid frontier, taken whole in one
// window, that decides a cold sync's shape, not this number.
constexpr int kSyncWindowBlocks = 25000;
constexpr int kSyncWindowBudgetMs = 5000;

// WHAT A CANCEL LEAVES BEHIND, in the words the module guarantees. Not a
// reassurance written by a view: `UtxoIndexer::sync_to` persists `synced_block`
// before each window returns, so every window that completed is on disk and a
// later step — or a later launch — resumes from there. A shield that is already
// mined is the chain's, owned by this wallet's 0zk address, and the next sync of
// any length finds it. Cancelling after a shield therefore leaves a shielded
// balance and no transfer, which is a state the wallet can show and spend from.
QString cancelNote(qint64 keptToBlock)
{
    return QStringLiteral(
               "Stopped at block %1 — nothing was rolled back. A shield that is already "
               "mined is untouched: it leaves a shielded balance and no transfer, and the "
               "next sync resumes from here.")
        .arg(keptToBlock);
}

} // namespace

// `sync_status` costs one `eth_blockNumber` and walks nothing, which is what
// makes it safe to ask before a send is offered rather than after: "this device
// is 2 200 blocks behind" is the difference between a send that is instant and
// one that is four minutes.
//
// IT DOES NOT START THE WALK. That is `startPrivateSync`, and the split is the
// answer to #235's fourth question — whether the sync should run in the
// background before the user asks to send. Knowing the distance is one RPC and
// is taken automatically; WALKING it is minutes of chain traffic on a phone's
// radio for a user who may never send privately, so it stays a thing the user
// asks for. Once asked for it does run in the background: the windows chain on
// through tab switches and the rest of the wallet stays usable.
void WalletUiWebBackend::refreshPrivateSync()
{
    announce(QStringLiteral("refreshPrivateSync: asking %1, %2").arg(kRailgun, doorState()));
    logos::web::callModuleAsync(
        kRailgun, QStringLiteral("sync_status"), QJsonArray{},
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                // A REFUSAL BY THE DOOR THIS EARLY IS A RACE, NOT AN ANSWER —
                // the same one `refreshAccounts` retries through, and the same
                // words ("token not recognized"). `res.ok` false is the door's
                // own failure; a module that answered `{ok:false}` has decided,
                // and is published straight away.
                if (!res.ok && m_privateSyncRetries++ < kAdmissionRetries) {
                    announce(QStringLiteral("%1 refused sync_status (%2/%3): %4")
                                 .arg(kRailgun)
                                 .arg(m_privateSyncRetries)
                                 .arg(kAdmissionRetries)
                                 .arg(res.error));
                    QTimer::singleShot(kAdmissionRetryMs, this,
                                       [this]() { refreshPrivateSync(); });
                    return;
                }
                privateSyncUnavailable(QStringLiteral("sync_status"), res, reply);
                return;
            }
            m_privateSyncRetries = 0;
            m_syncPlan = reply;
            // A WALK IN FLIGHT OUTRANKS THE READ. `sync_status` answers
            // `running` for a plan the MODULE holds; this variant knows whether
            // it is the one still asking for windows, and that is what the view
            // needs to decide between a Sync button and a Cancel button.
            if (m_syncRunning) {
                publishPrivateSync(kStateRunning, reply);
                return;
            }
            const bool done = reply.value(QStringLiteral("done")).toBool();
            publishPrivateSync(done ? kStateDone : kStateIdle, reply);
        });
}

QString WalletUiWebBackend::startPrivateSync()
{
    if (m_syncRunning) {
        // NOT a second walk. Two chains of windows against a `single`-concurrency
        // module would queue behind each other and double the traffic for one
        // plan, and the view would see a percentage flip between two readings of
        // it.
        return failed(QStringLiteral("A private sync is already running"));
    }
    m_syncRunning = true;
    m_syncCancelled = false;
    setStatusText(QStringLiteral("Syncing the private balance…"));
    publishPrivateSync(kStateRunning, m_syncPlan);
    stepPrivateSync();
    return accepted();
}

void WalletUiWebBackend::stepPrivateSync()
{
    QJsonObject params;
    params.insert(QStringLiteral("blocks"), kSyncWindowBlocks);
    params.insert(QStringLiteral("budgetMs"), kSyncWindowBudgetMs);
    // A JSON STRING, not an object. `sync_step(params_json: String)` takes its
    // document as text and refuses an object by name — which cost a device run
    // to find (#235), and is how every rust-first module on this wire is asked.
    logos::web::callModuleAsync(
        kRailgun, QStringLiteral("sync_step"), QJsonArray{ jsonText(params) },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                m_syncRunning = false;
                syncLegEnded(false, privateSyncUnavailable(QStringLiteral("sync_step"), res, reply));
                return;
            }
            m_syncPlan = reply;

            // THE CANCEL LANDS HERE. `cancelPrivateSync` cannot unmake the
            // window that was already in flight, and does not try to: it marks
            // the walk as left, and this is where the next one is not asked for.
            if (m_syncCancelled) {
                m_syncRunning = false;
                // The `sync_cancel` reply normally gets here first and has the
                // better number to report; whichever lands first ends the leg,
                // and `syncLegEnded` makes the second one a no-op.
                syncLegEnded(false,
                             cancelNote(reply.value(QStringLiteral("syncedBlock"))
                                            .toVariant()
                                            .toLongLong()));
                return;
            }
            if (reply.value(QStringLiteral("done")).toBool()) {
                m_syncRunning = false;
                setStatusText(QStringLiteral("Private balance is up to date"));
                publishPrivateSync(kStateDone, reply);
                syncLegEnded(true, QString());
                return;
            }
            // NOTHING MOVED. The module says so rather than letting a caller
            // loop on it: an engine that could not pass this block will not pass
            // it on the next turn either, and a spin is worse than a stall.
            if (reply.value(QStringLiteral("stalled")).toBool()) {
                m_syncRunning = false;
                const QString stalled =
                    QStringLiteral("The sync stopped making progress at block %1. "
                                   "Everything up to there is saved; try again later.")
                        .arg(reply.value(QStringLiteral("syncedBlock")).toVariant().toLongLong());
                publishPrivateSync(kStateIdle, reply, stalled);
                syncLegEnded(false, stalled);
                return;
            }
            publishPrivateSync(kStateRunning, reply);
            stepPrivateSync();
        });
}

// STOP ASKING — and say what that left. There is nothing to roll back, so this
// is not an undo and does not pretend to be one: `sync_cancel` drops the pinned
// target so the next step plans against a fresh head, and answers how far the
// cancelled walk got. See `cancelNote` for the state it leaves.
QString WalletUiWebBackend::cancelPrivateSync()
{
    m_syncCancelled = true;
    setStatusText(QStringLiteral("Stopping the private sync…"));
    logos::web::callModuleAsync(
        kRailgun, QStringLiteral("sync_cancel"), QJsonArray{},
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            m_syncRunning = false;
            if (!callSucceeded(res, reply)) {
                syncLegEnded(false,
                             privateSyncUnavailable(QStringLiteral("sync_cancel"), res, reply));
                return;
            }
            m_syncPlan = reply;
            // `keptToBlock` is only on a cancel that HAD a plan to drop; a cancel
            // with none still reports where the engine stands, which is the same
            // number under the plan's own name.
            const qint64 kept =
                reply.contains(QStringLiteral("keptToBlock"))
                    ? reply.value(QStringLiteral("keptToBlock")).toVariant().toLongLong()
                    : reply.value(QStringLiteral("syncedBlock")).toVariant().toLongLong();
            const QString note = cancelNote(kept);
            setStatusText(QStringLiteral("Private sync stopped"));
            publishPrivateSync(kStateCancelled, reply, note);
            syncLegEnded(false, note);
        });
    return accepted();
}

// A WALK NOBODY IS WAITING ON SIMPLY HAS NO CONTINUATION, which is the ordinary
// case: the Private tab's own "Sync now" ends when it ends. A walk a SEND
// started is that send's first leg, and this is the hand-over.
//
// CLEARED BEFORE IT RUNS. Two replies can reach an end at the same moment — the
// `sync_cancel` the user asked for and the window that was already in flight
// when they asked — and a send carried on twice is a second `relayed_send`
// against a `single`-concurrency module.
void WalletUiWebBackend::syncLegEnded(bool ok, const QString& why)
{
    if (!m_syncThen)
        return;
    const std::function<void(bool, const QString&)> then = m_syncThen;
    m_syncThen = nullptr;
    then(ok, why);
}

void WalletUiWebBackend::publishPrivateSync(const QString& state, const QJsonObject& plan,
                                            const QString& note, const QString& error)
{
    // THE MODULE'S OWN FIELDS, PASSED THROUGH. `startBlock`, `syncedBlock`,
    // `targetBlock`, `blocksTotal/Done/Remaining`, `percent`, `windows`,
    // `elapsedMs`, `etaMs` and `fastForwardTo` are what `Plan::to_json` wrote;
    // copying them rather than recomputing any is what keeps the bar the engine's
    // answer and not a second opinion about it.
    QJsonObject sync = plan;
    sync.remove(QStringLiteral("ok"));
    // THE LEG, NAMED. #235 asks a view to say which leg of a private send is
    // running — wrap / approve / shield / sync / prove / broadcast. `sync` is
    // the one that takes the minutes and the only one `railgun_module` reports
    // progress for today; the other five are short calls in its API and have no
    // window to show.
    sync.insert(QStringLiteral("leg"), QStringLiteral("sync"));
    sync.insert(QStringLiteral("state"), state);
    if (!note.isEmpty())
        sync.insert(QStringLiteral("note"), note);
    if (!error.isEmpty())
        sync.insert(QStringLiteral("error"), error);

    QJsonObject out;
    out.insert(QStringLiteral("sync"), sync);
    setPrivateSyncJson(jsonText(out));
    announce(QStringLiteral("private sync %1: %2").arg(state, jsonText(sync)));
}

QString WalletUiWebBackend::privateSyncUnavailable(const QString& method,
                                                   const logos::web::ModuleCallResult& res,
                                                   const QJsonObject& reply)
{
    const QString why = refusalReason(res, reply);
    // NAMED, ALWAYS. A build that ships no railgun_module and a railgun_module
    // that has not been initialised are the same blank tab to a user and two
    // different fixes to a developer, so the module and its own words both go on
    // the line.
    //
    // AND THE SECOND OF THOSE GETS THE SENTENCE IT NEEDS (#250). "not
    // initialized (call init first)" is accurate and tells a reader nothing they
    // can act on -- it was what the operator's iPad showed, and the obvious
    // reading (a broken module) is the wrong one. The module is fine; the
    // private wallet has never been brought up, because bringing it up means a
    // deterministic EOA signature the keystore only produces behind a human
    // approval, and this variant does not drive that route yet.
    const QString said =
        why.contains(QStringLiteral("not initialized"))
            ? QStringLiteral("%1 has no private wallet on this device yet: its keys are "
                             "derived from a signature by this account, and the `web` "
                             "variant does not ask for that approval yet (it refused %2: %3)")
                  .arg(kRailgun, method, why)
            : QStringLiteral("%1 refused %2: %3").arg(kRailgun, method, why);
    announce(said);
    setStatusText(said);
    publishPrivateSync(kStateUnavailable, QJsonObject{}, QString(), said);
    return said;
}

// ── the private send ─────────────────────────────────────────────────────────
//
// THE SEND IS THE ISSUE; THE SYNC ABOVE IS ONE LEG OF IT. Measured end to end
// on an iPad Air 13-inch simulator, a RAILGUN private send was ~154 s and on a
// physical iPad Air 4 it was 239 s, of which the accumulator walk was 221 s
// (logos-workspace#235). Nothing on screen said which part was running, nothing
// could be stopped, and a waiter with a fixed timeout under three minutes
// reported a failure for work that went on to succeed. This is the route made
// visible: four legs, one call outstanding at any moment, and a cancel whose
// meaning is stated per leg rather than implied.
//
// WHY `relayed_send` AND NOT `prepare_transfer`. `prepare_transfer` hands back
// an unsigned `transact(...)` for the caller to sign, broadcast and PAY FOR —
// out of the very public account the private send exists to keep out of it.
// `relayed_send` is the whole send inside the module: it builds the 7702
// UserOperation, proves it, puts the digests in front of a human through
// `keystore_module`, and submits the approved operation to the bundler through
// `eth_rpc_module`. So the wallet drives a send with two methods and a poll,
// and every leg boundary is a reply that landed rather than a guess.
//
// FOUR LEGS AND NOT SIX. `wrap` / `approve` / `shield` put funds INTO the pool
// and are a separate operation with a separate surface — see "the shield" at
// the bottom of this file, which is where the signing path lives. The route
// below is the send of funds that are ALREADY shielded, and it names its own
// four legs and no others: a route showing three permanently-skipped legs
// would be describing something this wallet never does in one go.
namespace {

// HOW OFTEN A PARKED APPROVAL IS ASKED ABOUT. A human in another app is the
// thing being waited for, so this is paced for a person and not for a chain: a
// second is imperceptible to the user who is approving and cheap on the wire,
// and the call itself is `approval_status` — one lookup, no chain read — until
// the moment it is approved, when the same call does the submission.
constexpr int kApprovalPollMs = 1000;

// WHAT LEAVING COSTS, at the two legs where leaving is possible and means
// something different. Written here, next to each other, because they are the
// wallet's answer to #235's second clause and are meant to be read as a pair.
const QString kCancelledBeforeSigning = QStringLiteral(
    "Stopped before anything was signed or broadcast — the approval request was withdrawn "
    "from the Signer app. Nothing reached the chain, and a shield that was already mined is "
    "untouched: it stays a shielded balance you can spend.");

// The one leg where there is nothing to stop.
const QString kAlreadyBroadcast = QStringLiteral(
    "This private send has already been broadcast: the operation is the chain's now and "
    "cannot be recalled.");

// What a send needs before this variant will spend a round trip on it, in the
// order a user fills them in. Same rule the custom-token field follows: what
// the wallet can see is wrong it says HERE, and never as a refusal that names a
// module as though the module were the problem.
QString missingSendField(const QJsonObject& p)
{
    if (p.value(QStringLiteral("to")).toString().trimmed().isEmpty())
        return QStringLiteral("A private send needs a recipient (`to`) — a 0zk… address for a "
                              "private transfer, or a 0x… address to unshield to.");
    if (p.value(QStringLiteral("asset")).toString().trimmed().isEmpty())
        return QStringLiteral("A private send needs the asset (`asset`) — the ERC-20 address "
                              "held in the shielded pool.");
    if (p.value(QStringLiteral("amount")).toString().trimmed().isEmpty())
        return QStringLiteral("A private send needs an amount (`amount`), in the asset's own "
                              "base units.");
    if (p.value(QStringLiteral("owner")).toString().trimmed().isEmpty())
        return QStringLiteral("A private send needs the account that signs it (`owner`) — the "
                              "EOA whose signature authorises the relayed operation.");
    if (p.value(QStringLiteral("bundlerUrl")).toString().trimmed().isEmpty())
        return QStringLiteral("A private send needs a bundler URL (`bundlerUrl`) — the ERC-4337 "
                              "bundler the signed operation is submitted to.");
    return QString();
}

} // namespace

void WalletUiWebBackend::resetSendRoute()
{
    m_sendLeg.clear();
    m_sendLegs = pendingRoute({ kLegSync, kLegProve, kLegApprove, kLegBroadcast });
}

void WalletUiWebBackend::setSendLeg(const QString& leg, const QString& state)
{
    markLeg(m_sendLegs, m_sendLeg, leg, state);
}

QString WalletUiWebBackend::sendLegState(const QString& leg) const
{
    for (const QJsonValue& v : m_sendLegs) {
        const QJsonObject entry = v.toObject();
        if (entry.value(QStringLiteral("name")).toString() == leg)
            return entry.value(QStringLiteral("state")).toString();
    }
    return QString();
}

QString WalletUiWebBackend::startPrivateSend(QString sendJson)
{
    if (m_sendRunning)
        return failed(QStringLiteral("A private send is already running"));

    const QJsonObject params = QJsonDocument::fromJson(sendJson.toUtf8()).object();
    const QString missing = missingSendField(params);
    if (!missing.isEmpty()) {
        setStatusText(missing);
        return failed(missing);
    }

    m_sendParams = QJsonObject{
        { QStringLiteral("to"), params.value(QStringLiteral("to")).toString().trimmed() },
        { QStringLiteral("asset"), params.value(QStringLiteral("asset")).toString().trimmed() },
        { QStringLiteral("amount"), params.value(QStringLiteral("amount")).toString().trimmed() },
        { QStringLiteral("memo"), params.value(QStringLiteral("memo")).toString() },
        { QStringLiteral("owner"), params.value(QStringLiteral("owner")).toString().trimmed() },
        { QStringLiteral("bundlerUrl"),
          params.value(QStringLiteral("bundlerUrl")).toString().trimmed() },
    };
    m_sendRequestId.clear();
    m_sendUserOpHash.clear();
    m_sendCancelled = false;
    m_sendRunning = true;
    resetSendRoute();

    // LEG 1 — THE WALK, AND WHO STARTS IT. #235's fourth clause asks whether
    // the sync should run in the background before a send. The split this
    // wallet makes: the DISTANCE is read automatically because it is one
    // `eth_blockNumber` and is what lets the tab warn before a send is offered;
    // the WALK is minutes of a phone's radio, so it happens when something
    // needs the tree — and the thing that needs the tree is a send.
    if (m_syncPlan.value(QStringLiteral("done")).toBool()) {
        // LEVEL ALREADY, and the wallet knows it without asking because the
        // distance is read automatically. `skipped`, not `done`: this send
        // walked nothing, and a route that claimed otherwise would be claiming
        // work. No continuation is set here at all — there is no walk to wait
        // on, and one left behind would carry a finished send onward the next
        // time the user pressed "Sync now".
        setSendLeg(kLegSync, kStateSkipped);
        beginSendProve();
        return accepted();
    }

    setSendLeg(kLegSync, kStateRunning);
    m_syncThen = [this](bool ok, const QString& why) {
        if (ok) {
            setSendLeg(kLegSync, kStateDone);
            beginSendProve();
            return;
        }
        // A WALK THAT DID NOT FINISH ends the send where it stands, and `why`
        // is filed under the heading it belongs to: what the user chose is a
        // `note`, what went wrong is an `error`.
        if (m_sendCancelled) {
            setSendLeg(kLegSync, kStateCancelled);
            finishSend(kStateCancelled, why);
        } else {
            setSendLeg(kLegSync, kStateFailed);
            finishSend(kStateFailed, QString(), why);
        }
    };
    publishPrivateSend(kStateRunning);

    if (m_syncRunning) {
        // A WALK IS ALREADY RUNNING — the user asked for it on the tab, or an
        // earlier ask has not finished. It is ADOPTED rather than duplicated:
        // `railgun_module` is `concurrency: single`, so a second chain of
        // windows against the same plan would queue behind the first and spend
        // the radio twice for one tree. The continuation above is all this send
        // needs; the walk that is running will run it.
        announce(QStringLiteral("private send: taking the walk already running as its sync leg"));
        return accepted();
    }
    startPrivateSync();
    return accepted();
}

// LEG 2 — the proof. `relayed_send` is one call that does several minutes'
// worth of nothing visible: it iterates a UserOperation against the bundler,
// generates the witness, proves it with Groth16 and lodges the approval
// request. There is no window inside it to report, which is exactly why the leg
// is NAMED — "prove, running" is the difference between a wallet that is
// working and a wallet that has hung.
void WalletUiWebBackend::beginSendProve()
{
    setSendLeg(kLegProve, kStateRunning);
    setStatusText(QStringLiteral("Proving the private send…"));
    publishPrivateSend(kStateRunning);
    // THE SEND'S APPROVAL IS LODGED BY railgun_module, NOT HERE, which is why
    // this route does not name an approver the way the shield's does. The same
    // session-scoped-roles finding applies to it (see nameAnApprover) and the
    // fix belongs with whoever owns this route's legs -- noted on
    // logos-workspace#245 rather than half-made here.
    logos::web::callModuleAsync(
        kRailgun, QStringLiteral("relayed_send"), QJsonArray{ jsonText(m_sendParams) },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failSendLeg(kLegProve,
                            moduleRefused(kRailgun, QStringLiteral("relayed_send"), res, reply));
                return;
            }
            m_sendRequestId = reply.value(QStringLiteral("requestId")).toString();
            setSendLeg(kLegProve, kStateDone);
            // THE USER LEFT WHILE IT WAS PROVING. There was no request to
            // withdraw until this reply landed, and now there is one — so the
            // cancel that could not be served then is served here.
            if (m_sendCancelled) {
                withdrawSendRequest();
                return;
            }
            setSendLeg(kLegApprove, kStateRunning);
            setStatusText(QStringLiteral("Waiting for approval in the Signer app"));
            publishPrivateSend(kStateRunning);
            pollSendApproval();
        });
}

// LEG 3 — the human, and LEG 4 inside its last reply. `relayed_send_status`
// answers `awaiting_approval` while the request is unanswered; the call that
// finds it approved is the one that fetches the signatures, puts them into the
// operation and submits it to the bundler. So `broadcast` is reported by its
// result and never as `running`: this side cannot see it start, and a leg shown
// as running for work that may not have begun is the lie this whole surface
// exists to avoid.
void WalletUiWebBackend::pollSendApproval()
{
    logos::web::callModuleAsync(
        kRailgun, QStringLiteral("relayed_send_status"), QJsonArray{ m_sendRequestId },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failSendLeg(kLegApprove,
                            moduleRefused(kRailgun, QStringLiteral("relayed_send_status"),
                                          res, reply));
                return;
            }
            const QString state = reply.value(QStringLiteral("state")).toString();
            if (state == QStringLiteral("done")) {
                m_sendUserOpHash = reply.value(QStringLiteral("userOpHash")).toString();
                setSendLeg(kLegApprove, kStateDone);
                setSendLeg(kLegBroadcast, kStateDone);
                setStatusText(QStringLiteral("Private send submitted"));
                finishSend(kStateDone,
                           QStringLiteral("Submitted to the bundler as %1.").arg(m_sendUserOpHash));
                return;
            }
            if (state == QStringLiteral("declined")) {
                // NOT A FAILURE OF THE WALLET, and named as what it is: a
                // person, or the keystore's sweep, said no. Nothing was
                // broadcast and nothing was spent.
                const QString reason = reply.value(QStringLiteral("reason")).toString();
                failSendLeg(kLegApprove,
                            QStringLiteral("The Signer app did not approve this send: %1")
                                .arg(reason.isEmpty() ? QStringLiteral("declined") : reason));
                return;
            }
            if (state != QStringLiteral("awaiting_approval")) {
                failSendLeg(
                    kLegApprove,
                    QStringLiteral("%1 answered an approval state this wallet does not know: %2")
                        .arg(kRailgun, state));
                return;
            }
            if (m_sendCancelled) {
                withdrawSendRequest();
                return;
            }
            // ASKED AGAIN, ON A TIMER, and the timer is the only place in this
            // route where a call is not chained out of a reply — because what
            // is being waited for is a person, and there is no reply to chain
            // out of until they answer.
            QTimer::singleShot(kApprovalPollMs, this, [this]() {
                if (m_sendRunning && !m_sendCancelled)
                    pollSendApproval();
            });
        });
}

// A LEG THAT COULD NOT GO ON, said the same way wherever it happens: on the
// page's console, on the status line, on the leg itself and on the send. Four
// things had drifted apart across the route's failure paths — an unknown
// approval state left the status line reading "Waiting for approval" for a send
// that had already ended — so they are done here, together, once.
void WalletUiWebBackend::failSendLeg(const QString& leg, const QString& why)
{
    announce(why);
    setStatusText(why);
    setSendLeg(leg, kStateFailed);
    finishSend(kStateFailed, QString(), why);
}

void WalletUiWebBackend::withdrawSendRequest()
{
    logos::web::callModuleAsync(
        kRailgun, QStringLiteral("relayed_send_cancel"), QJsonArray{ m_sendRequestId },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            setSendLeg(kLegApprove, kStateCancelled);
            if (!callSucceeded(res, reply)) {
                // THE SEND IS STILL CANCELLED. A withdrawal that did not land
                // leaves a request in the approver's queue, which the keystore
                // sweeps on its own — so the outcome for the user is the same
                // and the difference is said rather than hidden.
                const QString why =
                    moduleRefused(kRailgun, QStringLiteral("relayed_send_cancel"), res, reply);
                announce(why);
                finishSend(kStateCancelled, kCancelledBeforeSigning, why);
                return;
            }
            setStatusText(QStringLiteral("Private send cancelled"));
            finishSend(kStateCancelled, kCancelledBeforeSigning);
        });
}

// LEAVING, AND WHAT IT COSTS AT THE LEG THE SEND IS ON. The route keeps exactly
// one call outstanding, so there is always exactly one thing to not do next —
// and the note that lands says what was and was not done, on the surface the
// user is looking at.
QString WalletUiWebBackend::cancelPrivateSend()
{
    // Checked before "is anything running", because a finished send is not
    // running and the honest answer to cancelling one is not "there is nothing
    // here" — it is that the operation has left.
    if (sendLegState(kLegBroadcast) == kStateDone)
        return failed(kAlreadyBroadcast);
    if (!m_sendRunning)
        return failed(QStringLiteral("No private send is running"));

    m_sendCancelled = true;
    if (m_sendLeg == kLegSync) {
        // The walk's own cancel, which is not an undo and does not need to be:
        // every window it finished is persisted. `syncLegEnded` then ends the
        // send with the block the walk kept.
        cancelPrivateSync();
        return accepted();
    }
    if (!m_sendRequestId.isEmpty()) {
        withdrawSendRequest();
        return accepted();
    }
    // The proof is in flight and has not lodged a request yet. There is nothing
    // to withdraw and nothing to roll back; the reply will find the flag and
    // withdraw whatever it created.
    setStatusText(QStringLiteral("Stopping the private send…"));
    publishPrivateSend(kStateRunning, QStringLiteral("Stopping once the proof answers…"));
    return accepted();
}

void WalletUiWebBackend::publishPrivateSend(const QString& state, const QString& note,
                                            const QString& error)
{
    QJsonObject send;
    send.insert(QStringLiteral("state"), state);
    send.insert(QStringLiteral("leg"), m_sendLeg);
    send.insert(QStringLiteral("legs"), m_sendLegs);
    // WHETHER THE BUTTON SHOULD BE THERE, decided here rather than by a view
    // reading five states and guessing: a send in flight can be left, and one
    // that has ended — broadcast, declined, cancelled or never started — cannot.
    send.insert(QStringLiteral("cancellable"), m_sendRunning);
    if (!m_sendRequestId.isEmpty())
        send.insert(QStringLiteral("requestId"), m_sendRequestId);
    if (!m_sendUserOpHash.isEmpty())
        send.insert(QStringLiteral("userOpHash"), m_sendUserOpHash);
    if (!note.isEmpty())
        send.insert(QStringLiteral("note"), note);
    if (!error.isEmpty())
        send.insert(QStringLiteral("error"), error);

    QJsonObject out;
    out.insert(QStringLiteral("send"), send);
    setPrivateSendJson(jsonText(out));
    announce(QStringLiteral("private send %1: %2").arg(state, jsonText(send)));
}

void WalletUiWebBackend::finishSend(const QString& state, const QString& note,
                                    const QString& error)
{
    m_sendRunning = false;
    publishPrivateSend(state, note, error);
}

// ── the shield: public funds going INTO the pool ─────────────────────────────
//
// THE OTHER HALF OF #235 CLAUSE 1. The route above spends a balance that is
// already shielded and names four legs; the clause names six, and the three it
// was missing — `wrap`, `approve`, `shield` — are these. They were missing for
// a reason: every one of them is a PUBLIC transaction that has to be signed by
// the user's EOA and broadcast, and this variant had no signing path.
//
// IT HAS ONE, AND IT IS NOT A NEW MODULE. `keystore_module` — already here for
// the account list — takes an INTENT through `request_approval`: an address, a
// purpose, and legs to sign. `relayed_send` inside `railgun_module` uses the
// same door for the digests of its UserOperation, so this is the established
// way a module gets a signature out of this keystore, and the wallet holds no
// key and takes no password at any point.
//
// ONE BUNDLE, ONE HUMAN ANSWER. The keystore signs an intent's legs IN ORDER
// UNDER A SINGLE KEY DERIVATION, so the wrap, the allowance and the shield are
// rendered to the approver together and cost one password entry. They are
// signed with CONSECUTIVE NONCES, which is what lets all three be broadcast
// back to back: the chain will not execute the shield before the allowance,
// whatever order they reach a mempool in. That is the difference between a
// route that takes three block times and one that takes one.
//
// WHERE THE CANCEL LINE IS. Up to the moment the first raw transaction is
// handed to eth_rpc there is nothing on chain: the approval request is
// withdrawn from the Signer's queue and the signatures — if they were already
// collected — are dropped and wiped. After it, the transactions are the
// chain's, they are nonce-ordered and they will be mined, so a cancel is
// REFUSED. That line is harder than the send's, and it is stated rather than
// discovered.
namespace {

// THE ROUTE, in the order it is walked. `plan` and `sign` are not in #235's
// list of legs and are named anyway: they are where a user waits for a reason
// that is not the chain's, and a surface whose whole point is "the app has not
// hung" cannot have unnamed waits in it.
const QString kLegPlan = QStringLiteral("plan");
const QString kLegSign = QStringLiteral("sign");
const QString kLegWrap = QStringLiteral("wrap");
const QString kLegAllow = QStringLiteral("approve");
const QString kLegShield = QStringLiteral("shield");

// WETH9's payable `deposit()`. The one selector this wallet spells out, because
// it is the one call in the route no module builds for it: `prepare_shield`
// answers the shield, `approve(spender,value)` is the ERC-20 standard, and this
// is how native ETH becomes the ERC-20 RAILGUN takes. The asset the caller
// named IS the wrapper — the wallet does not go looking for a chain's
// "wrapped base token", because guessing which contract a user meant is exactly
// the kind of help a signing surface must not offer.
const QString kDepositSelector = QStringLiteral("0xd0e30db0");
// `approve(address,uint256)`.
const QString kApproveSelector = QStringLiteral("0x095ea7b3");

// GAS LIMITS THIS WALLET STATES RATHER THAN ESTIMATES, and the reason is the
// route's own shape: `eth_estimateGas` for the SHIELD would be asked against a
// chain state where the allowance it spends does not exist yet, so it would be
// refused — and estimating some legs and not others would make the "Gas limit"
// line the approver reads mean two different things on one screen. Unused gas
// is refunded, so a generous constant costs the user nothing beyond having the
// balance to cover it; a low estimate costs them a mined revert.
constexpr int kWrapGas = 80000;     // WETH9 deposit() is ~45k
constexpr int kApproveGas = 100000; // an ERC-20 approve is ~46k
constexpr int kShieldGas = 900000;  // RailgunSmartWallet shield, one note

// A tip a chain's floor is usually happy with, and a ceiling with room for the
// base fee to double before the bundle lands. The same shape `live_send.rs`
// uses for the probe's own transactions — a wallet pays the base fee of the
// block it lands in, not this ceiling.
constexpr quint64 kPriorityFeeWei = 1000000000ull; // 1 gwei

// How often a parked approval and a pending receipt are asked about. Different
// numbers because they wait for different things: a human in another app
// answers in seconds, and a block is twelve.
constexpr int kShieldApprovalPollMs = 1000;
constexpr int kShieldReceiptPollMs = 3000;

// eth_rpc answers its quantities as `0x…`; this is the one place they become
// numbers. A nonce and a gas price both fit in 64 bits by construction (a
// nonce is a count and a gas price is wei-per-gas), which is why this is not
// the 256-bit path below.
quint64 quantity(const QJsonValue& v, bool* ok)
{
    QString hex = v.toString().trimmed();
    if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        hex = hex.mid(2);
    if (hex.isEmpty()) {
        *ok = false;
        return 0;
    }
    return hex.toULongLong(ok, 16);
}

QString hexQuantity(quint64 v)
{
    return QStringLiteral("0x%1").arg(v, 0, 16);
}

// A decimal amount in an asset's base units → the 32-byte big-endian hex an
// ERC-20 argument is encoded as. DONE BY HAND rather than through a 64-bit
// integer: an 18-decimal token passes 2^64 at 18.45 whole units, so anything
// that went via `toULongLong` would refuse — or, worse, truncate — an ordinary
// amount. Answers empty for anything that is not a decimal integer under
// 2^256, which is the only way this can fail.
QString abiUint256(const QString& decimal)
{
    const QString s = decimal.trimmed();
    if (s.isEmpty())
        return QString();
    quint8 be[32] = { 0 };
    for (const QChar& ch : s) {
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9'))
            return QString();
        int carry = ch.unicode() - '0';
        for (int i = 31; i >= 0; --i) {
            const int v = int(be[i]) * 10 + carry;
            be[i] = quint8(v & 0xff);
            carry = v >> 8;
        }
        if (carry != 0)
            return QString(); // more than 2^256 - 1
    }
    QString out;
    for (quint8 b : be)
        out += QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0'));
    return out;
}

// An address as a 32-byte ABI word. Answers empty for anything that is not 20
// hex bytes, which is what makes a mistyped spender a refusal here rather than
// an allowance granted to nobody.
QString abiAddress(const QString& address)
{
    QString hex = address.trimmed();
    if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        hex = hex.mid(2);
    if (hex.size() != 40)
        return QString();
    for (const QChar& ch : hex) {
        if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
              || (ch.toLower() >= QLatin1Char('a') && ch.toLower() <= QLatin1Char('f'))))
            return QString();
    }
    return QString(24, QLatin1Char('0')) + hex.toLower();
}

// What a shield needs before this variant will spend a round trip on it, in the
// order a user fills them in — the same rule `missingSendField` follows: what
// the wallet can see is wrong it says HERE, and never as a refusal that names a
// module as though the module were the problem.
QString missingShieldField(const QJsonObject& p)
{
    if (p.value(QStringLiteral("chainId")).toInt() <= 0)
        return QStringLiteral("A shield needs the chain it happens on (`chainId`).");
    if (p.value(QStringLiteral("owner")).toString().trimmed().isEmpty())
        return QStringLiteral("A shield needs the account that signs and pays for it "
                              "(`owner`) — the EOA the public transactions come from.");
    if (p.value(QStringLiteral("asset")).toString().trimmed().isEmpty())
        return QStringLiteral("A shield needs the asset (`asset`) — the ERC-20 address being "
                              "deposited into the shielded pool.");
    if (abiUint256(p.value(QStringLiteral("amount")).toString()).isEmpty())
        return QStringLiteral("A shield needs an amount (`amount`) in the asset's own base "
                              "units, written as a decimal whole number.");
    return QString();
}

// WHAT LEAVING COSTS, before anything has been handed to the chain.
const QString kShieldCancelledBeforeChain = QStringLiteral(
    "Stopped before any transaction reached the chain — the approval request was withdrawn "
    "from the Signer app and the signatures were discarded. Nothing was spent, no allowance "
    "was granted and nothing was shielded.");

// ...and the one point where there is nothing to stop. Harder than the send's,
// because three transactions are already nonce-ordered in a mempool.
const QString kShieldAlreadyOnChain = QStringLiteral(
    "These transactions have already been broadcast: they are the chain's now, they are "
    "ordered by nonce and they cannot be recalled.");

} // namespace

void WalletUiWebBackend::resetShieldRoute()
{
    m_shieldLeg.clear();
    m_shieldLegs = pendingRoute({ kLegPlan, kLegSign, kLegWrap, kLegAllow, kLegShield });
}

void WalletUiWebBackend::setShieldLeg(const QString& leg, const QString& state)
{
    markLeg(m_shieldLegs, m_shieldLeg, leg, state);
}

QString WalletUiWebBackend::startPrivateShield(QString shieldJson)
{
    if (m_shieldRunning)
        return failed(QStringLiteral("A shield is already running"));

    const QJsonObject params = QJsonDocument::fromJson(shieldJson.toUtf8()).object();
    const QString missing = missingShieldField(params);
    if (!missing.isEmpty()) {
        // ON THE SURFACE AND NOT ONLY ON THE STATUS LINE. Measured on an iPad
        // Air 13-inch simulator: a device with no account in its keystore
        // pressed `Shield`, the form had no `owner`, and `privateShieldJson`
        // was never published at all — so the one panel the user was looking at
        // said nothing while the route had already been refused
        // (logos-workspace#235).
        setStatusText(missing);
        // The route is laid out fresh rather than left as the last run's: the
        // refusal is about the shield the user is trying to start NOW, and an
        // `idle` state over a route still showing `done` legs reads as neither.
        m_shieldTxs = QJsonArray{};
        resetShieldRoute();
        publishPrivateShield(kStateIdle, QString(), missing);
        return failed(missing);
    }

    m_shieldParams = QJsonObject{
        { QStringLiteral("chainId"), params.value(QStringLiteral("chainId")).toInt() },
        { QStringLiteral("owner"), params.value(QStringLiteral("owner")).toString().trimmed() },
        { QStringLiteral("asset"), params.value(QStringLiteral("asset")).toString().trimmed() },
        { QStringLiteral("amount"), params.value(QStringLiteral("amount")).toString().trimmed() },
        { QStringLiteral("wrap"), params.value(QStringLiteral("wrap")).toBool() },
    };
    m_shieldTxs = QJsonArray{};
    m_shieldSigned.clear();
    m_shieldHandle.clear();
    m_shieldReceipt.clear();
    m_shieldSent = 0;
    m_shieldMined = 0;
    m_shieldCancelled = false;
    m_shieldBroadcast = false;
    m_shieldRunning = true;
    resetShieldRoute();

    setShieldLeg(kLegPlan, kStateRunning);
    setStatusText(QStringLiteral("Preparing the shield…"));
    publishPrivateShield(kStateRunning);

    const int chainId = m_shieldParams.value(QStringLiteral("chainId")).toInt();
    // CONFIGURE, THEN ASK — the rule every eth_rpc read in this file follows,
    // and the one a device run found the hard way: a nonce asked of an
    // unconfigured chain comes back "no configuration for chain N".
    ensureChainConfig(
        chainId, chainById(chainId).value(QStringLiteral("rpcUrl")).toString(), [this]() {
            QJsonObject ask;
            ask.insert(QStringLiteral("asset"),
                       m_shieldParams.value(QStringLiteral("asset")).toString());
            ask.insert(QStringLiteral("amount"),
                       m_shieldParams.value(QStringLiteral("amount")).toString());
            logos::web::callModuleAsync(
                kRailgun, QStringLiteral("prepare_shield"), QJsonArray{ jsonText(ask) },
                [this](const logos::web::ModuleCallResult& res) {
                    const QJsonObject reply = replyOf(res);
                    if (!callSucceeded(res, reply)) {
                        failShieldLeg(kLegPlan,
                                      moduleRefused(kRailgun, QStringLiteral("prepare_shield"),
                                                    res, reply));
                        return;
                    }
                    const QJsonArray txs = reply.value(QStringLiteral("txs")).toArray();
                    if (txs.isEmpty()) {
                        failShieldLeg(kLegPlan,
                                      QStringLiteral("%1 answered no transactions to shield")
                                          .arg(kRailgun));
                        return;
                    }
                    // Kept as the module wrote them. The `to` of the first is
                    // the RailgunSmartWallet, and is what the allowance names
                    // as its spender — read off the reply so a redeployed
                    // contract does not need this wallet rebuilt.
                    for (const QJsonValue& v : txs) {
                        QJsonObject tx = v.toObject();
                        tx.insert(QStringLiteral("leg"), kLegShield);
                        m_shieldTxs.append(tx);
                    }
                    shieldReadNonce();
                });
        });
    return accepted();
}

void WalletUiWebBackend::shieldReadNonce()
{
    const int chainId = m_shieldParams.value(QStringLiteral("chainId")).toInt();
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("get_transaction_count"),
        QJsonArray{ chainId, m_shieldParams.value(QStringLiteral("owner")).toString() },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failShieldLeg(kLegPlan,
                              moduleRefused(kEthRpc, QStringLiteral("get_transaction_count"),
                                            res, reply));
                return;
            }
            bool ok = false;
            m_shieldNonce = quantity(resultOf(reply), &ok);
            if (!ok) {
                failShieldLeg(kLegPlan,
                              QStringLiteral("%1 answered a nonce this wallet cannot read: %2")
                                  .arg(kEthRpc, resultOf(reply).toString()));
                return;
            }
            shieldReadGasPrice();
        });
}

void WalletUiWebBackend::shieldReadGasPrice()
{
    const int chainId = m_shieldParams.value(QStringLiteral("chainId")).toInt();
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("gas_price"), QJsonArray{ chainId },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failShieldLeg(kLegPlan,
                              moduleRefused(kEthRpc, QStringLiteral("gas_price"), res, reply));
                return;
            }
            bool ok = false;
            const quint64 gasPrice = quantity(resultOf(reply), &ok);
            if (!ok) {
                failShieldLeg(kLegPlan,
                              QStringLiteral("%1 answered a gas price this wallet cannot read: %2")
                                  .arg(kEthRpc, resultOf(reply).toString()));
                return;
            }
            m_shieldTip = kPriorityFeeWei;
            m_shieldMaxFee = gasPrice * 2 + m_shieldTip;

            const QString wrong = buildShieldTxs();
            if (!wrong.isEmpty()) {
                failShieldLeg(kLegPlan, wrong);
                return;
            }
            setShieldLeg(kLegPlan, kStateDone);
            // THE USER LEFT WHILE THE PLAN WAS BEING READ. Nothing has been
            // asked of a human and nothing signed, so there is nothing to
            // withdraw — the route simply stops before it costs anyone a
            // decision.
            if (m_shieldCancelled) {
                setShieldLeg(kLegSign, kStateCancelled);
                setStatusText(QStringLiteral("Shield cancelled"));
                finishShield(kStateCancelled, kShieldCancelledBeforeChain);
                return;
            }
            requestShieldApproval();
        });
}

// LAY THE ROUTE OUT AS TRANSACTIONS, in the order the chain must execute them:
// the wrap mints the ERC-20, the allowance lets the RailgunSmartWallet take it,
// and the shield takes it. The nonces are consecutive from the account's
// current one, which is what makes that order a fact rather than a hope.
QString WalletUiWebBackend::buildShieldTxs()
{
    const QString asset = m_shieldParams.value(QStringLiteral("asset")).toString();
    const QString amount = m_shieldParams.value(QStringLiteral("amount")).toString();
    const QString spender =
        m_shieldTxs.isEmpty() ? QString()
                              : m_shieldTxs.at(0).toObject().value(QStringLiteral("to")).toString();

    const QString spenderWord = abiAddress(spender);
    if (spenderWord.isEmpty())
        return QStringLiteral("%1 answered a shield addressed to `%2`, which is not an address "
                              "this wallet can grant an allowance to.")
            .arg(kRailgun, spender);
    const QString amountWord = abiUint256(amount);
    if (amountWord.isEmpty())
        return QStringLiteral("`%1` is not an amount in base units this wallet can encode.")
            .arg(amount);

    QJsonArray route;
    if (m_shieldParams.value(QStringLiteral("wrap")).toBool()) {
        // `deposit()` ON THE ASSET ITSELF, paid with the amount being shielded.
        // The wallet does not look up a chain's wrapped base token: the caller
        // named the asset, and a signing surface that substituted a different
        // contract for the one a user typed would be the worst kind of help.
        route.append(QJsonObject{ { QStringLiteral("leg"), kLegWrap },
                                  { QStringLiteral("to"), asset },
                                  { QStringLiteral("data"), kDepositSelector },
                                  { QStringLiteral("value"), amount },
                                  { QStringLiteral("gasLimit"), kWrapGas } });
    } else {
        // NOT `pending`: a leg that was not needed and a leg that ran are
        // different claims, and only one of them is work this wallet did.
        setShieldLeg(kLegWrap, kStateSkipped);
    }
    route.append(QJsonObject{ { QStringLiteral("leg"), kLegAllow },
                              { QStringLiteral("to"), asset },
                              { QStringLiteral("data"),
                                kApproveSelector + spenderWord + amountWord },
                              { QStringLiteral("value"), QStringLiteral("0") },
                              { QStringLiteral("gasLimit"), kApproveGas } });
    for (const QJsonValue& v : m_shieldTxs) {
        QJsonObject tx = v.toObject();
        tx.insert(QStringLiteral("gasLimit"), kShieldGas);
        route.append(tx);
    }

    quint64 nonce = m_shieldNonce;
    for (int i = 0; i < route.size(); ++i) {
        QJsonObject tx = route.at(i).toObject();
        tx.insert(QStringLiteral("nonce"), hexQuantity(nonce++));
        route.replace(i, tx);
    }
    m_shieldTxs = route;
    return QString();
}

// LEG 2 — the human, asked ONCE for the whole route. The keystore renders every
// leg it is given, so the approver sees the wrap, the allowance and the shield
// on one screen with their selectors and their full calldata, and answers with
// one password. This wallet never sees the key or the password.
void WalletUiWebBackend::requestShieldApproval()
{
    const int chainId = m_shieldParams.value(QStringLiteral("chainId")).toInt();
    QJsonArray legs;
    for (const QJsonValue& v : m_shieldTxs) {
        const QJsonObject tx = v.toObject();
        QJsonObject unsignedTx;
        unsignedTx.insert(QStringLiteral("to"), tx.value(QStringLiteral("to")).toString());
        unsignedTx.insert(QStringLiteral("value"), tx.value(QStringLiteral("value")).toString());
        unsignedTx.insert(QStringLiteral("nonce"), tx.value(QStringLiteral("nonce")).toString());
        unsignedTx.insert(QStringLiteral("gas_limit"),
                          hexQuantity(quint64(tx.value(QStringLiteral("gasLimit")).toInt())));
        unsignedTx.insert(QStringLiteral("data"), tx.value(QStringLiteral("data")).toString());
        unsignedTx.insert(QStringLiteral("fee_mode"), QStringLiteral("eip1559"));
        unsignedTx.insert(QStringLiteral("max_fee_per_gas"), hexQuantity(m_shieldMaxFee));
        unsignedTx.insert(QStringLiteral("max_priority_fee_per_gas"), hexQuantity(m_shieldTip));
        legs.append(QJsonObject{ { QStringLiteral("kind"), QStringLiteral("tx") },
                                 { QStringLiteral("chain_id"), chainId },
                                 { QStringLiteral("tx"), unsignedTx } });
    }

    QJsonObject intent;
    intent.insert(QStringLiteral("address"),
                  m_shieldParams.value(QStringLiteral("owner")).toString());
    // A CLAIM AND THE KEYSTORE SAYS SO. The approver shows this under "claimed
    // by the requester" and shows the transactions separately, which is why it
    // can be plain and useful rather than careful: what is actually signed is
    // rendered from the legs and not from this line.
    intent.insert(QStringLiteral("purpose"),
                  QStringLiteral("Shield %1 of %2 into the RAILGUN pool")
                      .arg(m_shieldParams.value(QStringLiteral("amount")).toString(),
                           m_shieldParams.value(QStringLiteral("asset")).toString()));
    intent.insert(QStringLiteral("legs"), legs);

    setShieldLeg(kLegSign, kStateRunning);
    setStatusText(QStringLiteral("Waiting for approval in the Signer app"));
    publishPrivateShield(kStateRunning);
    // THE ROLES FIRST, AND EVERY TIME. See nameAnApprover: the keystore's roles
    // are session-scoped, so on any launch that did not itself create an account
    // the approver is the built-in `evm_signer_ui` — a module this image does
    // not carry — and this request would park for ever with nobody allowed to
    // answer it. That is what kept `wrap` / `approve` / `shield` off a chain.
    nameAnApprover([this, intent]() { lodgeShieldApproval(intent); },
                   [this](const QString& why) { failShieldLeg(kLegSign, why); });
}

// ...and the ask itself, once an approver holds the role.
void WalletUiWebBackend::lodgeShieldApproval(const QJsonObject& intent)
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("request_approval"), QJsonArray{ jsonText(intent) },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failShieldLeg(kLegSign,
                              moduleRefused(kKeystore, QStringLiteral("request_approval"),
                                            res, reply));
                return;
            }
            m_shieldHandle = reply.value(QStringLiteral("handle")).toString();
            m_shieldReceipt = reply.value(QStringLiteral("receipt")).toString();
            if (m_shieldHandle.isEmpty() || m_shieldReceipt.isEmpty()) {
                failShieldLeg(kLegSign,
                              QStringLiteral("%1 took the request but answered no handle to "
                                             "collect it with")
                                  .arg(kKeystore));
                return;
            }
            // THE USER LEFT WHILE THE REQUEST WAS BEING LODGED. There was
            // nothing to withdraw until this reply landed, and now there is.
            if (m_shieldCancelled) {
                withdrawShieldRequest();
                return;
            }
            pollShieldApproval();
        });
}

void WalletUiWebBackend::pollShieldApproval()
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("approval_status"),
        QJsonArray{ m_shieldHandle, m_shieldReceipt },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failShieldLeg(kLegSign,
                              moduleRefused(kKeystore, QStringLiteral("approval_status"),
                                            res, reply));
                return;
            }
            const QString state = reply.value(QStringLiteral("state")).toString();
            if (state == QStringLiteral("offered") || state == QStringLiteral("rendered")) {
                if (m_shieldCancelled) {
                    withdrawShieldRequest();
                    return;
                }
                // ASKED AGAIN, ON A TIMER — the only place in this route where
                // a call is not chained out of a reply, because what is being
                // waited for is a person and there is no reply to chain out of
                // until they answer.
                QTimer::singleShot(kShieldApprovalPollMs, this, [this]() {
                    if (m_shieldRunning && !m_shieldCancelled)
                        pollShieldApproval();
                });
                return;
            }
            if (state != QStringLiteral("settled")) {
                failShieldLeg(kLegSign,
                              QStringLiteral("%1 answered an approval state this wallet does "
                                             "not know: %2")
                                  .arg(kKeystore, state));
                return;
            }
            const QString reason = reply.value(QStringLiteral("reason")).toString();
            if (reason != QStringLiteral("approved")) {
                // NOT A FAILURE OF THE WALLET: a person said no, or the
                // keystore swept a request nobody answered. Nothing was signed.
                failShieldLeg(kLegSign,
                              QStringLiteral("The Signer app did not approve this shield: %1")
                                  .arg(reason.isEmpty() ? QStringLiteral("settled") : reason));
                return;
            }
            if (m_shieldCancelled) {
                // APPROVED, BUT THE USER HAS ALREADY LEFT. The request is
                // settled so there is nothing to withdraw; the signatures are
                // simply never collected, and the keystore is told to wipe the
                // copy it is holding for a requester that is not coming.
                setShieldLeg(kLegSign, kStateCancelled);
                ackShieldSignatures();
                return;
            }
            collectShieldSignatures();
        });
}

void WalletUiWebBackend::collectShieldSignatures()
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("fetch_result"), QJsonArray{ m_shieldHandle, m_shieldReceipt },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failShieldLeg(kLegSign,
                              moduleRefused(kKeystore, QStringLiteral("fetch_result"), res, reply));
                return;
            }
            m_shieldSigned.clear();
            for (const QJsonValue& v : reply.value(QStringLiteral("signed")).toArray())
                m_shieldSigned.append(v.toString());
            // ONE SIGNATURE PER TRANSACTION OR NONE OF THEM. A bundle that came
            // back short would leave a route where the allowance is signed and
            // the shield is not — which on chain is an allowance granted for
            // nothing, and is worse than a refusal here.
            if (m_shieldSigned.size() != m_shieldTxs.size()) {
                failShieldLeg(kLegSign,
                              QStringLiteral("%1 signed %2 of this route's %3 transactions")
                                  .arg(kKeystore)
                                  .arg(m_shieldSigned.size())
                                  .arg(m_shieldTxs.size()));
                return;
            }
            setShieldLeg(kLegSign, kStateDone);
            if (m_shieldCancelled) {
                // SIGNED AND NEVER SENT. This is the last moment a cancel costs
                // nothing: the raw transactions are dropped here and the
                // keystore is told to erase its copy.
                m_shieldSigned.clear();
                ackShieldSignatures();
                return;
            }
            broadcastShieldTx();
        });
}

// LEGS 3-5 — the chain, and all of them handed over before any of them is
// followed. The nonces already order the execution, so waiting for a receipt
// between them would cost the user a block time per leg and buy nothing.
void WalletUiWebBackend::broadcastShieldTx()
{
    if (m_shieldSent >= m_shieldTxs.size()) {
        ackShieldSignatures();
        return;
    }
    const QJsonObject tx = m_shieldTxs.at(m_shieldSent).toObject();
    const QString leg = tx.value(QStringLiteral("leg")).toString();
    // THE POINT OF NO RETURN, marked BEFORE the call and not on its reply: once
    // this has left, a cancel that answered "stopped" would be describing a
    // transaction that is already in a mempool.
    m_shieldBroadcast = true;
    setShieldLeg(leg, kStateRunning);
    setStatusText(QStringLiteral("Broadcasting the %1…").arg(leg));
    publishPrivateShield(kStateRunning);

    const int chainId = m_shieldParams.value(QStringLiteral("chainId")).toInt();
    const int index = m_shieldSent;
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("send_raw_transaction"),
        QJsonArray{ chainId, m_shieldSigned.at(index) },
        [this, index, leg](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failShieldLeg(leg,
                              moduleRefused(kEthRpc, QStringLiteral("send_raw_transaction"),
                                            res, reply));
                return;
            }
            QJsonObject tx = m_shieldTxs.at(index).toObject();
            tx.insert(QStringLiteral("hash"), reply.value(QStringLiteral("hash")).toString());
            m_shieldTxs.replace(index, tx);
            ++m_shieldSent;
            broadcastShieldTx();
        });
}

void WalletUiWebBackend::ackShieldSignatures()
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("ack_result"), QJsonArray{ m_shieldHandle, m_shieldReceipt },
        [this](const logos::web::ModuleCallResult& res) {
            // A BARE BOOL, and a best effort either way: the keystore sweeps a
            // result nobody acknowledged on its own, so a refusal here changes
            // nothing about what is on chain and is announced rather than shown.
            announce(QStringLiteral("%1.ack_result -> %2")
                         .arg(kKeystore,
                              res.ok ? (res.value.toBool() ? QStringLiteral("erased")
                                                           : QStringLiteral("refused"))
                                     : res.error));
            if (m_shieldCancelled && !m_shieldBroadcast) {
                setStatusText(QStringLiteral("Shield cancelled"));
                finishShield(kStateCancelled, kShieldCancelledBeforeChain);
                return;
            }
            followShieldReceipt();
        });
}

// ...AND THEN WAIT FOR BLOCKS, which is the one wait on this route whose length
// the chain decides. A leg is `done` when the last of its transactions is
// mined, so the surface moves as the chain does.
void WalletUiWebBackend::followShieldReceipt()
{
    if (m_shieldMined >= m_shieldTxs.size()) {
        QStringList hashes;
        for (const QJsonValue& v : m_shieldTxs)
            hashes.append(v.toObject().value(QStringLiteral("hash")).toString());
        setStatusText(QStringLiteral("Shielded"));
        finishShield(kStateDone,
                     QStringLiteral("Mined: %1. The balance is in the shielded pool and the "
                                    "next private sync will find it.")
                         .arg(hashes.join(QStringLiteral(", "))));
        return;
    }
    const int index = m_shieldMined;
    const QJsonObject tx = m_shieldTxs.at(index).toObject();
    const QString leg = tx.value(QStringLiteral("leg")).toString();
    const QString hash = tx.value(QStringLiteral("hash")).toString();
    const int chainId = m_shieldParams.value(QStringLiteral("chainId")).toInt();
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("get_transaction_receipt"), QJsonArray{ chainId, hash },
        [this, leg, hash](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                failShieldLeg(leg,
                              moduleRefused(kEthRpc, QStringLiteral("get_transaction_receipt"),
                                            res, reply));
                return;
            }
            const QJsonValue result = resultOf(reply);
            if (!result.isObject()) {
                // NOT MINED YET. A null receipt is the ordinary answer for the
                // twelve seconds a block takes, and is not an error to report.
                QTimer::singleShot(kShieldReceiptPollMs, this, [this]() {
                    if (m_shieldRunning)
                        followShieldReceipt();
                });
                return;
            }
            const QString status = result.toObject().value(QStringLiteral("status")).toString();
            if (status != QStringLiteral("0x1")) {
                // A MINED REVERT. The gas is spent and the state did not
                // change, and it is a different thing from a call that never
                // went out — so it is said as what it is.
                failShieldLeg(leg,
                              QStringLiteral("The %1 transaction was mined but reverted (%2). "
                                             "Its gas was spent; nothing after it will be "
                                             "executed.")
                                  .arg(leg, hash));
                return;
            }
            ++m_shieldMined;
            // The leg is done once the LAST of its transactions is mined —
            // `prepare_shield` may answer more than one.
            const bool moreOfThisLeg =
                m_shieldMined < m_shieldTxs.size()
                && m_shieldTxs.at(m_shieldMined).toObject().value(QStringLiteral("leg")).toString()
                    == leg;
            if (!moreOfThisLeg)
                setShieldLeg(leg, kStateDone);
            publishPrivateShield(kStateRunning);
            followShieldReceipt();
        });
}

void WalletUiWebBackend::withdrawShieldRequest()
{
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("cancel_approval"),
        QJsonArray{ m_shieldHandle, m_shieldReceipt },
        [this](const logos::web::ModuleCallResult& res) {
            // THE SHIELD IS CANCELLED EITHER WAY. A withdrawal that did not
            // land leaves a request in the approver's queue, which the keystore
            // sweeps on its own after a minute — so the outcome for the user is
            // the same and the difference is announced rather than hidden.
            announce(QStringLiteral("%1.cancel_approval -> %2")
                         .arg(kKeystore,
                              res.ok ? (res.value.toBool() ? QStringLiteral("withdrawn")
                                                           : QStringLiteral("refused"))
                                     : res.error));
            setShieldLeg(kLegSign, kStateCancelled);
            setStatusText(QStringLiteral("Shield cancelled"));
            finishShield(kStateCancelled, kShieldCancelledBeforeChain);
        });
}

QString WalletUiWebBackend::cancelPrivateShield()
{
    // Checked before "is anything running", for the same reason the send's is:
    // the honest answer to cancelling something that has left is not "there is
    // nothing here".
    if (m_shieldBroadcast)
        return failed(kShieldAlreadyOnChain);
    if (!m_shieldRunning)
        return failed(QStringLiteral("No shield is running"));

    m_shieldCancelled = true;
    if (!m_shieldHandle.isEmpty()) {
        withdrawShieldRequest();
        return accepted();
    }
    // The plan is still being read. Nothing has been asked of a human, so there
    // is nothing to withdraw and the reply in flight will find the flag.
    setStatusText(QStringLiteral("Stopping the shield…"));
    publishPrivateShield(kStateRunning, QStringLiteral("Stopping once the plan answers…"));
    return accepted();
}

void WalletUiWebBackend::failShieldLeg(const QString& leg, const QString& why)
{
    announce(why);
    setStatusText(why);
    setShieldLeg(leg, kStateFailed);
    finishShield(kStateFailed, QString(), why);
}

void WalletUiWebBackend::publishPrivateShield(const QString& state, const QString& note,
                                              const QString& error)
{
    QJsonObject shield;
    shield.insert(QStringLiteral("state"), state);
    shield.insert(QStringLiteral("leg"), m_shieldLeg);
    // THE HASHES BELONG TO THE LEG THAT PRODUCED THEM, so a user looking at a
    // route can take the one they need to a block explorer without counting
    // transactions.
    QJsonArray legs;
    for (const QJsonValue& v : m_shieldLegs) {
        QJsonObject entry = v.toObject();
        QJsonArray hashes;
        for (const QJsonValue& t : m_shieldTxs) {
            const QJsonObject tx = t.toObject();
            if (tx.value(QStringLiteral("leg")).toString()
                    == entry.value(QStringLiteral("name")).toString()
                && tx.contains(QStringLiteral("hash")))
                hashes.append(tx.value(QStringLiteral("hash")));
        }
        if (!hashes.isEmpty())
            entry.insert(QStringLiteral("hashes"), hashes);
        legs.append(entry);
    }
    shield.insert(QStringLiteral("legs"), legs);
    // WHETHER THE BUTTON SHOULD BE THERE, decided here rather than by a view
    // reading six states and guessing. Harder than the send's: a shield stops
    // being cancellable the moment its first transaction is handed over, not
    // when the route ends.
    shield.insert(QStringLiteral("cancellable"), m_shieldRunning && !m_shieldBroadcast);
    if (!note.isEmpty())
        shield.insert(QStringLiteral("note"), note);
    if (!error.isEmpty())
        shield.insert(QStringLiteral("error"), error);

    QJsonObject out;
    out.insert(QStringLiteral("shield"), shield);
    setPrivateShieldJson(jsonText(out));
    announce(QStringLiteral("private shield %1: %2").arg(state, jsonText(shield)));
}

void WalletUiWebBackend::finishShield(const QString& state, const QString& note,
                                      const QString& error)
{
    m_shieldRunning = false;
    publishPrivateShield(state, note, error);
}
