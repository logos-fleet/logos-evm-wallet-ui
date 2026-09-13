#include "wallet_ui_web_backend.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonValue>

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

QString jsonText(const QJsonObject& obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
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
                       "does accounts and balances").arg(what, module);
    setStatusText(message);
    QJsonObject out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("error"), message);
    return jsonText(out);
}

// ── config ───────────────────────────────────────────────────────────────────

void WalletUiWebBackend::ensureChainConfig(int chainId, const QString& endpoint)
{
    if (m_chainsConfigured.contains(chainId) || endpoint.isEmpty())
        return;
    m_chainsConfigured.insert(chainId);

    QJsonObject cfg;
    cfg.insert(QStringLiteral("endpoint"), endpoint);
    logos::web::callModuleAsync(
        kEthRpc, QStringLiteral("set_chain_config"),
        QJsonArray{ chainId, jsonText(cfg) },
        [this, chainId](const logos::web::ModuleCallResult& res) {
            // A REFUSAL IS NOT CACHED. set_chain_config answers a bare bool, so
            // a `false` — or a call that never reached the module — has to
            // clear the mark or every later balance on this chain would be
            // asked of an endpoint eth_rpc was never told about.
            if (!res.ok || !res.value.toBool()) {
                m_chainsConfigured.remove(chainId);
                setStatusText(QStringLiteral("eth_rpc_module refused chain %1: %2")
                                  .arg(chainId)
                                  .arg(res.ok ? QStringLiteral("bad config")
                                              : res.error));
            }
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
    ensureChainConfig(chainId,
                      chainById(chainId).value(QStringLiteral("rpcUrl")).toString());

    setStatusText(QStringLiteral("Testing chain %1…").arg(chainId));
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
    return accepted();
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
    logos::web::callModuleAsync(
        kKeystore, QStringLiteral("new_account"), QJsonArray{ passphrase },
        [this](const logos::web::ModuleCallResult& res) {
            if (!res.ok) {
                setStatusText(QStringLiteral("keystore_module: %1").arg(res.error));
                return;
            }
            setStatusText(QStringLiteral("Account created"));
            refreshAccounts();
        });
    return accepted();
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
    ++m_balanceEpoch;
    m_balances = QJsonObject();
    m_balancePending = 0;
    setStatusText(QStringLiteral("Refreshing balances…"));
    announce(QStringLiteral("refreshBalances(%1): %2 chain(s), %3")
                 .arg(address)
                 .arg(m_chains.size())
                 .arg(doorState()));

    for (const QJsonValue& c : m_chains) {
        const QJsonObject o = c.toObject();
        const int chainId = o.value(QStringLiteral("chainId")).toInt();
        ensureChainConfig(chainId, o.value(QStringLiteral("rpcUrl")).toString());
        ++m_balancePending;
        fetchBalance(address, chainId,
                     o.value(QStringLiteral("nativeSymbol")).toString());
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

void WalletUiWebBackend::refreshMarket(QString address)
{
    Q_UNUSED(address)
    refuse(QStringLiteral("Market prices"), QStringLiteral("uniswap_module"));
    setMarketJson(QStringLiteral("{\"chains\":[]}"));
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
