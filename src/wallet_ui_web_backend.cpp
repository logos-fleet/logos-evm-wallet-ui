#include "wallet_ui_web_backend.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

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
// What uniswap puts in a price's `address` field for a chain's NATIVE asset;
// every other entry carries a real ERC-20 address. Not a display name — the
// chain list's `nativeSymbol` is that, and is what the item is labelled with.
const QString kNativeAsset = QStringLiteral("ETH");

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
QString custodianRoles()
{
    QJsonObject roles;
    roles.insert(QStringLiteral("approvers"), QStringLiteral("evm_signer_ui"));
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

// ONLY FOR A MODULE THAT REALLY IS NOT HERE. `wallet_backend_module` is the
// last one, which is why this variant talks to `eth_rpc_module`,
// `uniswap_module` and `token_list_module` directly and still cannot serve
// sends, fee estimation or history at all.
//
// IT IS NOT THE ANSWER TO "THIS METHOD IS NOT IMPLEMENTED YET", and #147 is what
// that mistake costs: `importMnemonic` refused with these words while
// `keystore_module` — a `web` variant on a phone, loaded and answering
// `list_accounts` in the same run — was right there. A reader was sent looking
// for a missing build that was never missing. A method this variant has simply
// not got to says so in its own words; only a missing module comes through here.
QString WalletUiWebBackend::refuse(const QString& what, const QString& module)
{
    const QString message =
        QStringLiteral("%1 needs %2, which has no mobile build — the `web` variant "
                       "does accounts, balances, prices and token lists")
            .arg(what, module);
    setStatusText(message);
    return failed(message);
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

bool WalletUiWebBackend::setProxyConfig(QString proxyJson)
{
    Q_UNUSED(proxyJson)
    setProxyStatus(QStringLiteral("Proxy config is wallet_backend_module's"));
    setStatusText(QStringLiteral("Proxy config needs wallet_backend_module"));
    return false;
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
        kKeystore, QStringLiteral("configure"), QJsonArray{ custodianRoles() },
        [this, then](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (keystoreRefused(QStringLiteral("configure"), res, reply))
                return;
            announce(QStringLiteral("custodians are now %1")
                         .arg(jsonText(reply.value(QStringLiteral("custodians")).toArray())));
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

void WalletUiWebBackend::refreshHistory(QString address)
{
    Q_UNUSED(address)
    refuse(QStringLiteral("History"), QStringLiteral("wallet_backend_module"));
    setHistoryJson(QStringLiteral("{\"history\":[]}"));
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
// WHY railgun_module IS NOT IN THIS VARIANT'S `web.dependencies`. The core
// resolves a module's declared dependencies before loading it and REFUSES a
// module whose list it cannot satisfy. `railgun_module` is a Bundled member an
// image carries only when it was asked for (`--bundle railgun_module`), and
// declaring it would mean a build without it could not load the wallet at
// all — trading every wallet build for a tab. So the private surface is
// DISCOVERED instead: the wallet asks, and a build that does not carry railgun
// answers a refusal this file publishes as `unavailable` with the reason in it.
// That is the same shape the catalog's derived floor takes for
// `token_list_module` (ADR 0009) — a control a user can see refusing, rather
// than a module that silently will not start.
//
// ONE WINDOW IN FLIGHT AT A TIME, always. The next `sync_step` is issued from
// inside the last one's reply, which is the rule every chained call in this
// file follows (logos_web_module_call.h: "a backend that needs a sequence
// chains it in the callbacks"), and here it also carries the cancel: with never
// more than one window outstanding, "stop" is simply "do not ask for another".
namespace {

// The module that owns the accumulator. Reached by name and not declared —
// see the section header for why.
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
            setStatusText(QStringLiteral("Private sync stopped"));
            publishPrivateSync(kStateCancelled, reply, cancelNote(kept));
            syncLegEnded(false, cancelNote(kept));
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
    const std::function<void(bool, QString)> then = m_syncThen;
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
    const QString said = QStringLiteral("%1 refused %2: %3").arg(kRailgun, method, why);
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
// an unsigned `transact(...)` for the caller to sign and broadcast, and this
// variant has no signing or broadcasting of its own — the coordinator owns
// sends and has no mobile build, which is why `sendNative` here refuses by
// name. `relayed_send` is the whole send inside the module: it builds the 7702
// UserOperation, proves it, puts the digests in front of a human through
// `keystore_module`, and submits the approved operation to the bundler through
// `eth_rpc_module`. So the wallet drives a send with two methods and a poll,
// and every leg boundary is a reply that landed rather than a guess.
//
// WHAT IS STILL NOT HERE, said plainly rather than faked: a SHIELD (public →
// private). `prepare_shield` answers with public transactions for the caller to
// approve and send, which needs exactly the signing path this variant does not
// have — so `wrap` / `approve` / `shield`, the three legs that put funds INTO
// the pool, belong to a build that can sign. The route below is the send of
// funds that are already shielded, and it names its own four legs and no others.
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
    m_sendLegs = QJsonArray{};
    for (const QString& leg : { kLegSync, kLegProve, kLegApprove, kLegBroadcast })
        m_sendLegs.append(QJsonObject{ { QStringLiteral("name"), leg },
                                       { QStringLiteral("state"), kStatePending } });
}

void WalletUiWebBackend::setSendLeg(const QString& leg, const QString& state)
{
    for (int i = 0; i < m_sendLegs.size(); ++i) {
        QJsonObject entry = m_sendLegs.at(i).toObject();
        if (entry.value(QStringLiteral("name")).toString() != leg)
            continue;
        entry.insert(QStringLiteral("state"), state);
        m_sendLegs.replace(i, entry);
        break;
    }
    // `leg` names what is RUNNING, so it is only moved by a leg that starts.
    // A leg that ends leaves it where it was, which is what lets a failed or
    // cancelled send still say which leg it was on.
    if (state == kStateRunning)
        m_sendLeg = leg;
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
        if (!ok) {
            const bool left = m_sendCancelled;
            setSendLeg(kLegSync, left ? kStateCancelled : kStateFailed);
            finishSend(left ? kStateCancelled : kStateFailed, left ? why : QString(),
                       left ? QString() : why);
            return;
        }
        setSendLeg(kLegSync, kStateDone);
        beginSendProve();
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
    logos::web::callModuleAsync(
        kRailgun, QStringLiteral("relayed_send"), QJsonArray{ jsonText(m_sendParams) },
        [this](const logos::web::ModuleCallResult& res) {
            const QJsonObject reply = replyOf(res);
            if (!callSucceeded(res, reply)) {
                const QString why = QStringLiteral("%1 refused relayed_send: %2")
                                        .arg(kRailgun, refusalReason(res, reply));
                announce(why);
                setStatusText(why);
                setSendLeg(kLegProve, kStateFailed);
                finishSend(kStateFailed, QString(), why);
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
                const QString why = QStringLiteral("%1 refused relayed_send_status: %2")
                                        .arg(kRailgun, refusalReason(res, reply));
                announce(why);
                setStatusText(why);
                setSendLeg(kLegApprove, kStateFailed);
                finishSend(kStateFailed, QString(), why);
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
                setSendLeg(kLegApprove, kStateFailed);
                const QString why =
                    QStringLiteral("The Signer app did not approve this send: %1")
                        .arg(reason.isEmpty() ? QStringLiteral("declined") : reason);
                setStatusText(why);
                finishSend(kStateFailed, QString(), why);
                return;
            }
            if (state != QStringLiteral("awaiting_approval")) {
                const QString why =
                    QStringLiteral("%1 answered an approval state this wallet does not know: %2")
                        .arg(kRailgun, state);
                announce(why);
                setSendLeg(kLegApprove, kStateFailed);
                finishSend(kStateFailed, QString(), why);
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
                const QString why = QStringLiteral("%1 refused relayed_send_cancel: %2")
                                        .arg(kRailgun, refusalReason(res, reply));
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
    // reading five states and guessing. A broadcast send is the one that is
    // over and cannot be taken back.
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
