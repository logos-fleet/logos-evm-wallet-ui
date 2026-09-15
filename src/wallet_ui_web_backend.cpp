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
// What uniswap puts in a price's `address` field for a chain's NATIVE asset;
// every other entry carries a real ERC-20 address. Not a display name — the
// chain list's `nativeSymbol` is that, and is what the item is labelled with.
const QString kNativeAsset = QStringLiteral("ETH");

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
constexpr int kAccountRetries = 6;
constexpr int kAccountRetryMs = 500;

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

} // namespace

WalletUiWebBackend::WalletUiWebBackend(QObject* parent)
    : WalletUiSimpleSource(parent)
{
    seedDefaultChains();
    publishChains();
    setAccountsJson(QStringLiteral("{\"accounts\":[]}"));
    setBalancesJson(QStringLiteral("{\"balances\":{\"chains\":[]}}"));
    setStatusText(QStringLiteral("Ready — balances through eth_rpc_module"));

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
            QTimer::singleShot(kAdmissionSettleMs, this,
                               [this]() { refreshAccounts(); });
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

QString WalletUiWebBackend::refuse(const QString& what, const QString& module)
{
    const QString message =
        QStringLiteral("%1 needs %2, which has no mobile build — the `web` variant "
                       "does accounts, balances and prices").arg(what, module);
    setStatusText(message);
    QJsonObject out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), message);
    return jsonText(out);
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
                if (m_accountRetries++ < kAccountRetries)
                    QTimer::singleShot(kAccountRetryMs, this,
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
    if (res.ok && reply.value(QStringLiteral("ok")).toBool())
        return false;
    // A call that never arrived and one the module turned down are the same
    // outcome to the caller but not the same reason, so the reason is taken from
    // whichever it was. The same words then go to the console and to the view's
    // status line: a device run reads the first and a human reads the second.
    const QString why = res.ok ? errorOf(reply) : res.error;
    announce(QStringLiteral("keystore_module refused %1: %2").arg(method, why));
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

QString WalletUiWebBackend::importMnemonic(QString phraseJson, QString label)
{
    Q_UNUSED(phraseJson)
    Q_UNUSED(label)
    return refuse(QStringLiteral("Mnemonic import"), kKeystore);
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

// ── everything the coordinator owns ──────────────────────────────────────────

void WalletUiWebBackend::loadTokens(int chainId)
{
    Q_UNUSED(chainId)
    refuse(QStringLiteral("Token lists"), QStringLiteral("token_list_module"));
    setTokensJson(QStringLiteral("{\"tokens\":[]}"));
}

bool WalletUiWebBackend::addCustomToken(QString tokenJson)
{
    Q_UNUSED(tokenJson)
    refuse(QStringLiteral("Custom tokens"), QStringLiteral("token_list_module"));
    return false;
}

// ── market: the Bundled uniswap_module (#148) ───────────────────────────────
//
// THE SECOND MODULE THIS VARIANT REACHES BY NAME, and the same two-step shape as
// a balance: configure the chain on eth_rpc, then ask. The ask goes to UNISWAP,
// which issues its own Multicall3 `eth_call` back through eth_rpc — so the
// endpoint has to be in eth_rpc before the price is asked for, exactly as it
// does before a balance, and for the reason ensureChainConfig's note gives.
//
// WHAT IT PRICES, and why the token list is empty. `token_list_module` has no
// mobile build, so this image holds no list of a user's ERC-20s and passes
// `{"tokens":[]}` — which is not an empty question: `get_prices` always reports
// the chain's native asset, anchored on the chain's stablecoins through the
// pools it derives offline. So the Market tab shows a real ETH price on every
// chain uniswap has a deployment for, and gains the user's tokens for free the
// day token_list crosses (this call then takes the list it publishes).
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

            QJsonArray items;
            QString failure;
            if (!res.ok) {
                failure = res.error;
            } else {
                const QJsonObject reply = replyOf(res);
                // A refusal is carried in uniswap's own words, which on a chain
                // it has no deployment for are "no uniswap config for chain
                // <id>" — a far more useful line than an empty list.
                if (reply.value(QStringLiteral("ok")).toBool())
                    items = priceItems(reply.value(QStringLiteral("prices")).toArray(),
                                       chainId, symbol);
                else
                    failure = errorOf(reply);
            }

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
