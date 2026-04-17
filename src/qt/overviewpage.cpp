#include <QDebug>
#include <QFile>
#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStatusTipEvent>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>
#include <map>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#define DECORATION_SIZE 54
#define NUM_ITEMS 5

Q_DECLARE_METATYPE(interfaces::WalletBalances)

namespace {
QProcess* g_minerProcess = nullptr;
bool g_isMining = false;
QString g_latestHashrate = "0 H/s";
QTimer* g_hashrateTimer = nullptr;
std::vector<OverviewPage*> g_overviewPages;
}

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr)
        : QAbstractItemDelegate(parent), platformStyle(_platformStyle)
    {
        connect(this, &TxViewDelegate::width_changed, this, [this](const QModelIndex& index) {
    Q_EMIT sizeHintChanged(index);
});
    }

    inline void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2 * ypad) / 2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top() + ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top() + ypad + halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if (value.canConvert<QBrush>()) {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        if (index.data(TransactionTableModel::WatchonlyRole).toBool()) {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(addressRect.left(), addressRect.top(), 16, addressRect.height());
            iconWatchonly = platformStyle->TextColorIcon(iconWatchonly);
            iconWatchonly.paint(painter, watchonlyRect);
            addressRect.setLeft(addressRect.left() + watchonlyRect.width() + 5);
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if (amount < 0) {
            foreground = COLOR_NEGATIVE;
        } else if (!confirmed) {
            foreground = COLOR_UNCONFIRMED;
        } else {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        if (!confirmed) {
            amountText = QString("[") + amountText + QString("]");
        }

        QRect amountBoundingRect;
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText, &amountBoundingRect);

        painter->setPen(option.palette.color(QPalette::Text));
        QRect dateBoundingRect;
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date), &dateBoundingRect);

        const int minimumWidth = static_cast<int>(1.4 * dateBoundingRect.width() + amountBoundingRect.width());
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimumWidth) {
            m_minimum_width[index.row()] = minimumWidth;
            Q_EMIT width_changed(index);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimumTextWidth = search == m_minimum_width.end() ? 0 : search->second;
        return {DECORATION_SIZE + 8 + minimumTextWidth, DECORATION_SIZE};
    }

    BitcoinUnit unit{BitcoinUnit::BTC};

Q_SIGNALS:
    void width_changed(const QModelIndex& index) const;

private:
    const PlatformStyle* platformStyle;
    mutable std::map<int, int> m_minimum_width;
};

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle* platformStyle, QWidget* parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    m_platform_style{platformStyle},
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);

    ui->labelHashRate->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->labelConnections->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->labelBlockHeight->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ui->miningStatusLabel->setTextFormat(Qt::RichText);

    g_overviewPages.push_back(this);

    connect(ui->startMiningButton, &QPushButton::clicked, this, [this]() {
        if (!g_isMining) {
            if (g_minerProcess) {
                g_minerProcess->deleteLater();
                g_minerProcess = nullptr;
            }

            g_minerProcess = new QProcess();
            g_minerProcess->setProcessChannelMode(QProcess::MergedChannels);

#ifdef Q_OS_WIN
            g_minerProcess->setCreateProcessArgumentsModifier(
                [](QProcess::CreateProcessArguments* args) {
                    args->flags |= CREATE_NO_WINDOW;
                });
#endif

            QString program = QCoreApplication::applicationDirPath() + "/gbt_miner.exe";

qDebug() << "矿工路径：" << program;
qDebug() << "是否存在：" << QFile::exists(program);

            QStringList arguments;
            arguments << "--rpcport=28476"
                      << "--rpcuser=user"
                      << "--rpcpassword=pass"
                      << "--submit-to=127.0.0.1:28476";

            QObject::connect(g_minerProcess, &QProcess::readyRead, this, [this]() {
                if (!g_minerProcess) return;

                const QString output = QString::fromUtf8(g_minerProcess->readAll());
                if (output.isEmpty()) return;

                QRegularExpression regex1(R"(hash(?:ing)?[^0-9]*([0-9]+(?:\.[0-9]+)?)\s*(?:/s|H/s|KH/s|MH/s|GH/s|TH/s))",
                                          QRegularExpression::CaseInsensitiveOption);
                QRegularExpression regex2(R"(([0-9]+(?:\.[0-9]+)?\s*[kKmMgGtT]?\s*H/s))");
                QRegularExpression regex3(R"(([0-9]+(?:\.[0-9]+)?\s*/s))");

                QRegularExpressionMatch match = regex1.match(output);
                if (match.hasMatch()) {
                    g_latestHashrate = match.captured(1) + " H/s";
                    OverviewPage::refreshAllMiningUi();
                    return;
                }

                match = regex2.match(output);
                if (match.hasMatch()) {
                    g_latestHashrate = match.captured(1).simplified();
                    OverviewPage::refreshAllMiningUi();
                    return;
                }

                match = regex3.match(output);
                if (match.hasMatch()) {
                    g_latestHashrate = match.captured(1).simplified();
                    OverviewPage::refreshAllMiningUi();
                }
            });

            QObject::connect(g_minerProcess,
                             QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                             this,
                             [](int, QProcess::ExitStatus) {
                                 g_isMining = false;
                                 g_latestHashrate = "0 H/s";

                                 if (g_hashrateTimer) {
                                     g_hashrateTimer->stop();
                                     delete g_hashrateTimer;
                                     g_hashrateTimer = nullptr;
                                 }

                                 if (g_minerProcess) {
                                     g_minerProcess->deleteLater();
                                     g_minerProcess = nullptr;
                                 }

                                 OverviewPage::refreshAllMiningUi();
                             });

            g_minerProcess->start(program, arguments);

            if (!g_minerProcess->waitForStarted(10000)) {
                if (g_minerProcess) {
                    g_minerProcess->deleteLater();
                    g_minerProcess = nullptr;
                }
                g_isMining = false;
                g_latestHashrate = "0 H/s";
                OverviewPage::refreshAllMiningUi();
                return;
            }

            if (g_hashrateTimer) {
                g_hashrateTimer->stop();
                delete g_hashrateTimer;
                g_hashrateTimer = nullptr;
            }

            g_hashrateTimer = new QTimer();
            QObject::connect(g_hashrateTimer, &QTimer::timeout, this, []() {
                OverviewPage::refreshAllMiningUi();
            });
            g_hashrateTimer->start(1000);

            g_latestHashrate = "reading...";
            g_isMining = true;
        } else {
            if (g_hashrateTimer) {
                g_hashrateTimer->stop();
                delete g_hashrateTimer;
                g_hashrateTimer = nullptr;
            }

            if (g_minerProcess) {
                g_minerProcess->terminate();
                if (!g_minerProcess->waitForFinished(3000)) {
                    g_minerProcess->kill();
                    g_minerProcess->waitForFinished(1000);
                }
                g_minerProcess->deleteLater();
                g_minerProcess = nullptr;
            }

#ifdef Q_OS_WIN
            QProcess::execute("taskkill", QStringList() << "/F" << "/IM" << "gbt_miner.exe");
#endif

            g_latestHashrate = "0 H/s";
            g_isMining = false;
        }

        OverviewPage::refreshAllMiningUi();
    });

    QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);

    updateMiningUI();
}

OverviewPage::~OverviewPage()
{
    g_overviewPages.erase(std::remove(g_overviewPages.begin(), g_overviewPages.end(), this), g_overviewPages.end());
    delete ui;
}

void OverviewPage::handleTransactionClicked(const QModelIndex& index)
{
    if (filter) {
        Q_EMIT transactionClicked(filter->mapToSource(index));
    }
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    if (clientModel && clientModel->getOptionsModel()) {
        clientModel->getOptionsModel()->setOption(OptionsModel::OptionID::MaskValues, privacy);
    }
    if (walletModel) {
        const auto& balances = walletModel->getCachedBalance();
        if (balances.balance != -1) {
            setBalance(balances);
        }
    }

    ui->listTransactions->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the Overview tab. To unmask the values, uncheck Settings->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    if (!walletModel || !walletModel->getOptionsModel()) {
        return;
    }

    BitcoinUnit unit = walletModel->getOptionsModel()->getDisplayUnit();
    if (walletModel->wallet().isLegacy()) {
        if (walletModel->wallet().privateKeysDisabled()) {
            ui->labelBalance->setText(BitcoinUnits::formatWithPrivacy(unit, balances.watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelUnconfirmed->setText(BitcoinUnits::formatWithPrivacy(unit, balances.unconfirmed_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelImmature->setText(BitcoinUnits::formatWithPrivacy(unit, balances.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelTotal->setText(BitcoinUnits::formatWithPrivacy(unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        } else {
            ui->labelBalance->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelUnconfirmed->setText(BitcoinUnits::formatWithPrivacy(unit, balances.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelImmature->setText(BitcoinUnits::formatWithPrivacy(unit, balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelTotal->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchAvailable->setText(BitcoinUnits::formatWithPrivacy(unit, balances.watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchPending->setText(BitcoinUnits::formatWithPrivacy(unit, balances.unconfirmed_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchImmature->setText(BitcoinUnits::formatWithPrivacy(unit, balances.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchTotal->setText(BitcoinUnits::formatWithPrivacy(unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        }
    } else {
        ui->labelBalance->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelUnconfirmed->setText(BitcoinUnits::formatWithPrivacy(unit, balances.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelImmature->setText(BitcoinUnits::formatWithPrivacy(unit, balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelTotal->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    }

    bool showImmature = balances.immature_balance != 0;
    bool showWatchOnlyImmature = balances.immature_watch_only_balance != 0;

    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(!walletModel->wallet().privateKeysDisabled() && showWatchOnlyImmature);
}

void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);
    ui->labelWatchonly->setVisible(showWatchOnly);
    ui->lineWatchBalance->setVisible(showWatchOnly);
    ui->labelWatchAvailable->setVisible(showWatchOnly);
    ui->labelWatchPending->setVisible(showWatchOnly);
    ui->labelWatchTotal->setVisible(showWatchOnly);

    if (!showWatchOnly) {
        ui->labelWatchImmature->hide();
    }
}

void OverviewPage::setClientModel(ClientModel* model)
{
    this->clientModel = model;

    if (model) {
        connect(model, &ClientModel::numConnectionsChanged, this, &OverviewPage::updateConnections, Qt::UniqueConnection);
        connect(model, &ClientModel::numBlocksChanged, this, &OverviewPage::updateBlockHeight, Qt::UniqueConnection);
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts, Qt::UniqueConnection);
        updateAlerts(model->getStatusBarWarnings());

        updateConnections(model->getNumConnections());
        ui->labelBlockHeight->setText(tr("Block Height: %1").arg(model->getNumBlocks()));

        if (model->getOptionsModel()) {
            connect(model->getOptionsModel(), &OptionsModel::fontForMoneyChanged,
                    this, &OverviewPage::setMonospacedFont, Qt::UniqueConnection);
            setMonospacedFont(model->getOptionsModel()->getFontForMoney());
        }
    } else {
        ui->labelConnections->setText(tr("Connections: 0"));
        ui->labelBlockHeight->setText(tr("Block Height: 0"));
    }

    updateMiningUI();
}

void OverviewPage::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
    if (model && model->getOptionsModel()) {
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        connect(filter.get(), &TransactionFilterProxy::rowsInserted, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsRemoved, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsMoved, this, &OverviewPage::LimitTransactionRows);
        LimitTransactionRows();

        setBalance(model->getCachedBalance());
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);
        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);

        interfaces::Wallet& wallet = model->wallet();
        updateWatchOnlyLabels(wallet.haveWatchOnly() && !wallet.privateKeysDisabled());
        connect(model, &WalletModel::notifyWatchonlyChanged, [this](bool showWatchOnly) {
            updateWatchOnlyLabels(showWatchOnly && !walletModel->wallet().privateKeysDisabled());
        });
    }

    updateDisplayUnit();
    updateMiningUI();
}

void OverviewPage::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
        ui->labelTransactionsStatus->setIcon(icon);
        ui->labelWalletStatus->setIcon(icon);
    }

    QWidget::changeEvent(e);
}

void OverviewPage::LimitTransactionRows()
{
    if (filter && ui->listTransactions && ui->listTransactions->model() && filter.get() == ui->listTransactions->model()) {
        for (int i = 0; i < filter->rowCount(); ++i) {
            ui->listTransactions->setRowHidden(i, i >= NUM_ITEMS);
        }
    }
}

void OverviewPage::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        const auto& balances = walletModel->getCachedBalance();
        if (balances.balance != -1) {
            setBalance(balances);
        }

        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();
        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString& warnings)
{
    ui->labelAlerts->setVisible(!warnings.isEmpty());
    ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::setMonospacedFont(const QFont& f)
{
    ui->labelBalance->setFont(f);
    ui->labelUnconfirmed->setFont(f);
    ui->labelImmature->setFont(f);
    ui->labelTotal->setFont(f);
    ui->labelWatchAvailable->setFont(f);
    ui->labelWatchPending->setFont(f);
    ui->labelWatchImmature->setFont(f);
    ui->labelWatchTotal->setFont(f);
}

void OverviewPage::updateConnections(int count)
{
    ui->labelConnections->setText(tr("Connections: %1").arg(count));
}

void OverviewPage::updateMiningUI()
{
    if (g_isMining) {
        ui->startMiningButton->setText(tr("Stop Mining"));
        ui->miningStatusLabel->setText(QStringLiteral("🟢 ") + tr("Mining..."));
    } else {
        ui->startMiningButton->setText(tr("Start Mining"));
        ui->miningStatusLabel->setText(QStringLiteral("🔴 ") + tr("Stopped"));
    }

    ui->labelHashRate->setText(QStringLiteral("⚡ ") + tr("Hash Rate: %1").arg(g_latestHashrate));

    if (clientModel) {
        ui->labelConnections->setText(QStringLiteral("🌐 ") + tr("Connections: %1").arg(clientModel->getNumConnections()));

        // 🟧 区块高度用橙色小方块
        ui->labelBlockHeight->setText(QStringLiteral("🟧 ") +
                                     tr("Block Height: %1").arg(clientModel->getNumBlocks()));
    }
}

void OverviewPage::refreshAllMiningUi()
{
    for (OverviewPage* page : g_overviewPages) {
        if (page) {
            page->updateMiningUI();
        }
    }
}

void OverviewPage::updateBlockHeight(int count,
                                     const QDateTime&,
                                     double,
                                     SyncType,
                                     SynchronizationState)
{
    ui->labelBlockHeight->setText(QStringLiteral("🟧 ") +
                                 tr("Block Height: %1").arg(count));
}
