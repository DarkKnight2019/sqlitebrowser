#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QUrl>
#include <QUrlQuery>

#include "RemoteDock.h"
#include "ui_RemoteDock.h"
#include "Settings.h"
#include "RemoteCommitsModel.h"
#include "RemoteDatabase.h"
#include "RemoteLocalFilesModel.h"
#include "RemoteModel.h"
#include "MainWindow.h"
#include "RemotePushDialog.h"
#include "PreferencesDialog.h"

RemoteDock::RemoteDock(MainWindow* parent)
    : QDialog(parent),
      ui(new Ui::RemoteDock),
      mainWindow(parent),
      remoteModel(new RemoteModel(this)),
      remoteLocalFilesModel(new RemoteLocalFilesModel(this, remoteDatabase)),
      remoteCommitsModel(new RemoteCommitsModel(this))
{
    ui->setupUi(this);

    // Set models
    ui->treeRemote->setModel(remoteModel);
    ui->treeLocal->setModel(remoteLocalFilesModel);
    ui->treeDatabaseCommits->setModel(remoteCommitsModel);

    // Set initial column widths for tree views
    ui->treeRemote->setColumnWidth(0, 300);                                     // Make name column wider
    ui->treeRemote->setColumnWidth(2, 80);                                      // Make size column narrower
    ui->treeLocal->setColumnWidth(RemoteLocalFilesModel::ColumnName, 300);      // Make name column wider
    ui->treeLocal->setColumnWidth(RemoteLocalFilesModel::ColumnSize, 80);       // Make size column narrower
    ui->treeLocal->setColumnHidden(RemoteLocalFilesModel::ColumnFile, true);    // Hide local file name

    // Handle finished uploads and downloads of databases
    connect(&RemoteNetwork::get(), &RemoteNetwork::fetchFinished, this, &RemoteDock::fetchFinished);
    connect(&RemoteNetwork::get(), &RemoteNetwork::pushFinished, this, &RemoteDock::pushFinished);

    // Whenever a new directory listing has been parsed, check if it was a new root dir and, if so, open the user's directory
    connect(remoteModel, &RemoteModel::directoryListingParsed, this, &RemoteDock::newDirectoryNode);

    // Show metadata for a database when we get it
    connect(&RemoteNetwork::get(), &RemoteNetwork::gotMetadata, this, &RemoteDock::showMetadata);

    // When the Preferences link is clicked in the no-certificates-label, open the preferences dialog. For other links than the ones we know,
    // just open them in a web browser
    connect(ui->labelNoCert, &QLabel::linkActivated, [this](const QString& link) {
        if(link == "#preferences")
        {
            PreferencesDialog dialog(mainWindow, PreferencesDialog::TabRemote);
            if(dialog.exec())
                mainWindow->reloadSettings();
        } else {
            QDesktopServices::openUrl(QUrl(link));
        }
    });

    // When changing the current branch in the branches combo box, update the tree view accordingly
    connect(ui->comboDatabaseBranch, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), [this](int /*index*/) {
        remoteCommitsModel->refresh(current_commit_json, ui->comboDatabaseBranch->currentData().toString().toStdString(), currently_opened_file_info.commit_id);
        ui->treeDatabaseCommits->expandAll();
    });

    // Fetch latest commit action
    connect(ui->actionFetchLatestCommit, &QAction::triggered, [this]() {
        // Fetch last commit of current branch
        // The URL and the branch name is that of the currently opened database file.
        // The latest commit id is stored in the data bits of the branch combo box.
        QUrl url(QString::fromStdString(currently_opened_file_info.url));
        QUrlQuery query;
        query.addQueryItem("branch", QString::fromStdString(currently_opened_file_info.branch));
        query.addQueryItem("commit", ui->comboDatabaseBranch->itemData(
                               ui->comboDatabaseBranch->findText(QString::fromStdString(currently_opened_file_info.branch))).toString());
        url.setQuery(query);
        fetchDatabase(url.toString());
    });

    // Prepare context menu for list of remote databases
    connect(ui->treeRemote->selectionModel(), &QItemSelectionModel::currentChanged, [this](const QModelIndex& index, const QModelIndex&) {
        // Only enable database actions when a database was selected
        bool enable = index.isValid() &&
                remoteModel->modelIndexToItem(index)->value(RemoteModelColumnType).toString() == "database";
        ui->actionCloneDatabaseDoubleClick->setEnabled(enable);
    });
    ui->treeRemote->selectionModel()->currentChanged(QModelIndex(), QModelIndex()); // Enable/disable all action initially
    connect(ui->actionCloneDatabaseDoubleClick, &QAction::triggered, [this]() {
       fetchDatabase(ui->treeRemote->currentIndex());
    });
    ui->treeRemote->addAction(ui->actionCloneDatabaseDoubleClick);

    // Prepare context menu for list of local clones
    connect(ui->treeLocal->selectionModel(), &QItemSelectionModel::currentChanged, [this](const QModelIndex& index, const QModelIndex&) {
        // Only enable database actions when a database was selected
        bool enable = index.isValid() &&
                !index.sibling(index.row(), RemoteLocalFilesModel::ColumnFile).data().isNull();
        ui->actionOpenLocalDatabase->setEnabled(enable);
        ui->actionDeleteDatabase->setEnabled(enable);
    });
    ui->treeLocal->selectionModel()->currentChanged(QModelIndex(), QModelIndex()); // Enable/disable all action initially
    connect(ui->actionOpenLocalDatabase, &QAction::triggered, [this]() {
       openLocalFile(ui->treeLocal->currentIndex());
    });
    connect(ui->actionDeleteDatabase, &QAction::triggered, [this]() {
       deleteLocalDatabase(ui->treeLocal->currentIndex());
    });
    ui->treeLocal->addAction(ui->actionOpenLocalDatabase);
    ui->treeLocal->addAction(ui->actionDeleteDatabase);

    // Prepare context menu for list of commits
    connect(ui->treeDatabaseCommits->selectionModel(), &QItemSelectionModel::currentChanged, [this](const QModelIndex& index, const QModelIndex&) {
        // Only enable database actions when a commit was selected
        bool enable = index.isValid();
        ui->actionFetchCommit->setEnabled(enable);
        ui->actionDownloadCommit->setEnabled(enable);
    });
    ui->treeDatabaseCommits->selectionModel()->currentChanged(QModelIndex(), QModelIndex()); // Enable/disable all action initially
    connect(ui->actionFetchCommit, &QAction::triggered, [this]() {
       fetchCommit(ui->treeDatabaseCommits->currentIndex());
    });
    connect(ui->actionDownloadCommit, &QAction::triggered, [this]() {
       fetchCommit(ui->treeDatabaseCommits->currentIndex(), RemoteNetwork::RequestTypeDownload);
    });
    ui->treeDatabaseCommits->addAction(ui->actionFetchCommit);
    ui->treeDatabaseCommits->addAction(ui->actionDownloadCommit);

    // Initial setup
    reloadSettings();
}

RemoteDock::~RemoteDock()
{
    delete ui;
}

void RemoteDock::reloadSettings()
{
    // Clear list of client certificates and add a dummy entry which does nothing except serve as
    // an explanation to the user.
    ui->comboUser->clear();
    ui->comboUser->addItem(tr("Select an identity to connect"), "dummy");

    // Load list of client certs
    QStringList client_certs = Settings::getValue("remote", "client_certificates").toStringList();
    for(const QString& file : client_certs)
    {
        auto certs = QSslCertificate::fromPath(file);
        for(const QSslCertificate& cert : certs)
            ui->comboUser->addItem(cert.subjectInfo(QSslCertificate::CommonName).at(0), file);
    }

    // Add public certificate for anonymous read-only access to dbhub.io
    ui->comboUser->addItem(tr("Public"), ":/user_certs/public.cert.pem");
}

void RemoteDock::setNewIdentity(const QString& identity)
{
    // Do nothing if the dummy entry was selected
    if(ui->comboUser->currentData() == "dummy")
        return;

    // Check if the dummy item is still there and remove it if it is
    if(ui->comboUser->itemData(0) == "dummy")
    {
        ui->comboUser->blockSignals(true);
        ui->comboUser->removeItem(0);
        ui->comboUser->blockSignals(false);
    }

    // Get certificate file name
    QString cert = ui->comboUser->itemData(ui->comboUser->findText(identity), Qt::UserRole).toString();
    if(cert.isEmpty())
        return;

    // Open root directory. Get host name from client cert
    remoteModel->setNewRootDir(RemoteNetwork::get().getInfoFromClientCert(cert, RemoteNetwork::CertInfoServer), cert);

    // Reset list of local checkouts
    remoteLocalFilesModel->setIdentity(cert);
    refreshLocalFileList();

    // Enable buttons if necessary
    enableButtons();
}

void RemoteDock::fetchDatabase(const QModelIndex& idx)
{
    if(!idx.isValid())
        return;

    // Get item
    const RemoteModelItem* item = remoteModel->modelIndexToItem(idx);

    // Only open database file
    if(item->value(RemoteModelColumnType).toString() == "database")
        fetchDatabase(item->value(RemoteModelColumnUrl).toString());
}

void RemoteDock::fetchDatabase(QString url_string, RemoteNetwork::RequestType request_type)
{
    // If no URL was provided ask the user. Default to the current clipboard contents
    if(url_string.isEmpty())
    {
        url_string = QInputDialog::getText(this,
                                           qApp->applicationName(),
                                           tr("This downloads a database from a remote server for local editing.\n"
                                              "Please enter the URL to clone from. You can generate this URL by\n"
                                              "clicking the 'Clone Database in DB4S' button on the web page\n"
                                              "of the database."),
                                           QLineEdit::Normal,
                                           QApplication::clipboard()->text());
    }

    if(url_string.isEmpty())
        return;

    // Check the URL
    QUrl url(url_string);
    if(url.authority() != QUrl(RemoteNetwork::get().getInfoFromClientCert(remoteModel->currentClientCertificate(), RemoteNetwork::CertInfoServer)).authority())
    {
        QMessageBox::warning(this, qApp->applicationName(), tr("Invalid URL: The host name does not match the host name of the current identity."));
        return;
    }
    if(!QUrlQuery(url).hasQueryItem("branch"))
    {
        QMessageBox::warning(this, qApp->applicationName(), tr("Invalid URL: No branch name specified."));
        return;
    }
    if(!QUrlQuery(url).hasQueryItem("commit"))
    {
        QMessageBox::warning(this, qApp->applicationName(), tr("Invalid URL: No commit ID specified."));
        return;
    }


    // There is a chance that we've already cloned that database. So check for that first
    QString exists = remoteDatabase.localExists(url, remoteModel->currentClientCertificate(), QUrlQuery(url).queryItemValue("branch").toStdString());
    if(!exists.isEmpty())
    {
        // Database has already been cloned! So open the local file instead of fetching the one from the
        // server again.
        emit openFile(exists);
        return;
    }

    // Check if we already have a clone of this database branch. If so, show a warning because there might
    // be unpushed changes. For this we don't care about the currently checked out commit id because for
    // any commit local changes could be lost.
    // TODO Detect local changes and don't warn when no changes were made
    QUrl url_without_commit_id(url);
    QUrlQuery url_without_commit_id_query(url_without_commit_id);
    url_without_commit_id_query.removeQueryItem("commit");
    url_without_commit_id.setQuery(url_without_commit_id_query);
    if(!remoteDatabase.localExists(url_without_commit_id, remoteModel->currentClientCertificate(), QUrlQuery(url).queryItemValue("branch").toStdString()).isEmpty())
    {
        if(QMessageBox::warning(nullptr,
                                QApplication::applicationName(),
                                tr("Fetching this commit might override local changes when you have not pushed them yet.\n"
                                   "Are you sure you want to fetch it?"),
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel) == QMessageBox::Cancel)
        {
            return;
        }
    }

    // Clone the database
    RemoteNetwork::get().fetch(url.toString(), request_type, remoteModel->currentClientCertificate());
}

void RemoteDock::fetchCommit(const QModelIndex& idx, RemoteNetwork::RequestType request_type)
{
    // Fetch selected commit
    QUrl url(QString::fromStdString(currently_opened_file_info.url));
    QUrlQuery query;
    query.addQueryItem("branch", ui->comboDatabaseBranch->currentText());
    query.addQueryItem("commit", idx.sibling(idx.row(), RemoteCommitsModel::ColumnCommitId).data().toString());
    url.setQuery(query);
    fetchDatabase(url.toString(), request_type);
}

void RemoteDock::enableButtons()
{
    bool db_opened = mainWindow->getDb().isOpen() && mainWindow->getDb().currentFile() != ":memory:";
    bool logged_in = !remoteModel->currentClientCertificate().isEmpty();

    ui->buttonPushDatabase->setEnabled(db_opened && logged_in);
    ui->actionRefresh->setEnabled(logged_in);
    ui->actionCloneDatabaseLink->setEnabled(logged_in);
    ui->actionDatabaseOpenBrowser->setEnabled(db_opened && logged_in);
    ui->actionFetchLatestCommit->setEnabled(db_opened && logged_in);
}

void RemoteDock::pushDatabase()
{
    // If the currently active identity is the read-only public access to dbhub.io, don't show the Push Database dialog because it won't work anyway.
    // Instead switch to an explanation offering some advice to create and import a proper certificate.
    if(remoteModel->currentClientCertificate() == ":/user_certs/public.cert.pem")
    {
        ui->stack->setCurrentIndex(1);
        return;
    }

    // Show a warning when trying to push a database with unsaved changes
    if(mainWindow->getDb().getDirty())
    {
        if(QMessageBox::warning(this,
                                QApplication::applicationName(),
                                tr("The database has unsaved changes. Are you sure you want to push it before saving?"),
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel) == QMessageBox::Cancel)
            return;
    }

    // The default suggestion for a database name is the local file name. If it is a remote file (like when it initially was fetched using DB4S),
    // the extra bit of information at the end of the name gets removed first.
    QString name = QFileInfo(mainWindow->getDb().currentFile()).fileName();
    name = name.remove(QRegExp("_[0-9]+.remotedb$"));

    // Show the user a dialog for setting all the commit details
    QString host = RemoteNetwork::get().getInfoFromClientCert(remoteModel->currentClientCertificate(), RemoteNetwork::CertInfoServer);
    RemotePushDialog pushDialog(this, host, remoteModel->currentClientCertificate(), name, QString::fromStdString(currently_opened_file_info.branch));
    if(pushDialog.exec() != QDialog::Accepted)
        return;

    // Build push URL
    QString url = host;
    url.append(RemoteNetwork::get().getInfoFromClientCert(remoteModel->currentClientCertificate(), RemoteNetwork::CertInfoUser));
    url.append("/");
    url.append(pushDialog.name());

    // Check if we are pushing a cloned database. Only in this case we provide the last known commit id
    QString commit_id;
    if(mainWindow->getDb().currentFile().startsWith(Settings::getValue("remote", "clonedirectory").toString()))
        commit_id = QString::fromStdString(remoteDatabase.localLastCommitId(remoteModel->currentClientCertificate(), url, pushDialog.branch().toStdString()));

    // Push database
    RemoteNetwork::get().push(mainWindow->getDb().currentFile(), url, remoteModel->currentClientCertificate(), pushDialog.name(),
                              pushDialog.commitMessage(), pushDialog.licence(), pushDialog.isPublic(), pushDialog.branch(),
                              pushDialog.forcePush(), commit_id);
}

void RemoteDock::newDirectoryNode(const QModelIndex& parent)
{
    // Was this a new root dir?
    if(!parent.isValid())
    {
        // Then check if there is a directory with the current user name

        // Get current user name
        QString user = RemoteNetwork::get().getInfoFromClientCert(remoteModel->currentClientCertificate(), RemoteNetwork::CertInfoUser);

        for(int i=0;i<remoteModel->rowCount();i++)
        {
            QModelIndex child = remoteModel->index(i, RemoteModelColumnName);
            if(child.data().toString() == user)
            {
                ui->treeRemote->expand(child);
                break;
            }
        }
    }
}

void RemoteDock::reject()
{
    // We override this, to ensure the Escape key doesn't make this dialog
    // dock go away
    return;
}

void RemoteDock::switchToMainView()
{
    ui->stack->setCurrentIndex(0);
}

void RemoteDock::refreshLocalFileList()
{
    remoteLocalFilesModel->refresh();

    // Expand node for current user
    QString user = RemoteNetwork::get().getInfoFromClientCert(remoteModel->currentClientCertificate(), RemoteNetwork::CertInfoUser);
    for(int i=0;i<remoteLocalFilesModel->rowCount();i++)
    {
        QModelIndex child = remoteLocalFilesModel->index(i, RemoteLocalFilesModel::ColumnName);
        if(child.data().toString() == user)
        {
            ui->treeLocal->expand(child);
            break;
        }
    }
}

void RemoteDock::openLocalFile(const QModelIndex& idx)
{
    if(!idx.isValid())
        return;

    QString file = idx.sibling(idx.row(), RemoteLocalFilesModel::ColumnFile).data().toString();
    if(!file.isEmpty())
        emit openFile(Settings::getValue("remote", "clonedirectory").toString() + "/" + file);
}

void RemoteDock::fileOpened(const QString& filename)
{
    // Clear data first
    currently_opened_file_info.clear();
    remoteCommitsModel->clear();
    ui->comboDatabaseBranch->clear();
    ui->editDatabaseUser->clear();
    ui->editDatabaseFile->clear();
    ui->editDatabaseBranch->clear();

    // Do nothing if the file name is empty (indicating a closed database) or this is an in-memory database
    if(filename.isEmpty() || filename == ":memory:")
        return;

    // Check if it is a tracked remote database file and retrieve the information we have on it
    if(filename.startsWith(Settings::getValue("remote", "clonedirectory").toString()))
        currently_opened_file_info = remoteDatabase.localGetLocalFileInfo(filename);

    // Is this actually a clone of a remote database?
    if(!currently_opened_file_info.file.empty())
    {
        // Copy information to view
        ui->editDatabaseUser->setText(currently_opened_file_info.user_name());
        ui->editDatabaseFile->setText(QString::fromStdString(currently_opened_file_info.name));
        ui->editDatabaseBranch->setText(QString::fromStdString(currently_opened_file_info.branch));

        // Make sure the current identity matches the identity used to clone this file in the first place.
        // A mismatch is possible when the local database file has been opened using a recent files menu item or some similar technique.
        if(QString::fromStdString(currently_opened_file_info.identity) != QFileInfo(remoteModel->currentClientCertificate()).fileName())
            ui->comboUser->setCurrentIndex(ui->comboUser->findData("/" + QString::fromStdString(currently_opened_file_info.identity), Qt::UserRole, Qt::MatchEndsWith));

        // Query more information on database from server
        refreshMetadata(currently_opened_file_info.user_name(), QString::fromStdString(currently_opened_file_info.name));

        // Switch to "Current Database" tab
        ui->tabs->setCurrentIndex(2);
    }
}

void RemoteDock::refreshMetadata(const QString& username, const QString& dbname)
{
    QUrl url(RemoteNetwork::get().getInfoFromClientCert(remoteModel->currentClientCertificate(), RemoteNetwork::CertInfoServer) + "/metadata/get");
    QUrlQuery query;
    query.addQueryItem("username", username);
    query.addQueryItem("folder", "/");
    query.addQueryItem("dbname", dbname);
    url.setQuery(query);
    RemoteNetwork::get().fetch(url.toString(), RemoteNetwork::RequestTypeMetadata, remoteModel->currentClientCertificate());
}

void RemoteDock::showMetadata(const std::vector<RemoteMetadataBranchInfo>& branches, const std::string& commits,
                              const std::vector<RemoteMetadataReleaseInfo>& /*releases*/, const std::vector<RemoteMetadataReleaseInfo>& /*tags*/,
                              const std::string& /*default_branch*/, const std::string& web_page)
{
    // Store all the commit information as-is
    current_commit_json = commits;

    // Store the link to the web page in the action for opening that link in a browser
    ui->actionDatabaseOpenBrowser->setData(QString::fromStdString(web_page));

    // Fill branches combo box
    ui->comboDatabaseBranch->clear();
    for(const auto& branch : branches)
        ui->comboDatabaseBranch->addItem(QString::fromStdString(branch.name), QString::fromStdString(branch.commit_id));
    ui->comboDatabaseBranch->setCurrentIndex(ui->comboDatabaseBranch->findText(ui->editDatabaseBranch->text()));
}

void RemoteDock::deleteLocalDatabase(const QModelIndex& index)
{
    if(!index.isValid())
        return;

    QString filename = index.sibling(index.row(), RemoteLocalFilesModel::ColumnFile).data().toString();
    QString path = Settings::getValue("remote", "clonedirectory").toString() + "/" + filename;

    // Warn when trying to delete a currently opened database file
    if(mainWindow->getDb().currentFile() == path)
    {
        QMessageBox::warning(this, QApplication::applicationName(), tr("The database you are trying to delete is currently opened. "
                                                                       "Please close it before deleting."));
        return;
    }

    // Let user confirm deleting the database
    if(QMessageBox::warning(this, QApplication::applicationName(), tr("This deletes the local version of this database with all the "
                                                                      "changes you have not committed yet. Are you sure you want to "
                                                                      "delete this database?"),
                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Cancel)
    {
        return;
    }

    // Delete the file
    remoteLocalFilesModel->removeRow(index.row(), index.parent());
}

void RemoteDock::openCurrentDatabaseInBrowser() const
{
    QDesktopServices::openUrl(ui->actionDatabaseOpenBrowser->data().toUrl());
}

void RemoteDock::refresh()
{
    // Refresh Remote tab
    remoteModel->refresh();

    // Refresh Local tab
    refreshLocalFileList();

    // Refresh Current Database tab
    if(!currently_opened_file_info.file.empty())
        refreshMetadata(currently_opened_file_info.user_name(), QString::fromStdString(currently_opened_file_info.name));
}

void RemoteDock::pushFinished(const QString& filename, const QString& identity, const QUrl& url, const std::string& new_commit_id,
                              const std::string& branch, const QString& source_file)
{
    // Create or update the record in our local checkout database
    QString saveFileAs = remoteDatabase.localAdd(filename, identity, url, new_commit_id, branch);

    // If the name of the source file and the name we're saving as differ, we're doing an initial push. In this case, copy the source file to
    // the destination path to avoid redownloading it when it's first used.
    if(saveFileAs != source_file)
        QFile::copy(source_file, saveFileAs);

    // Update info on currently opened file
    currently_opened_file_info = remoteDatabase.localGetLocalFileInfo(saveFileAs);

    // Refresh view
    refresh();
}

void RemoteDock::fetchFinished(const QString& filename, const QString& identity, const QUrl& url, const std::string& new_commit_id,
                               const std::string& branch, const QDateTime& last_modified, QIODevice* device)
{
    // Add cloned database to list of local databases
    QString saveFileAs = remoteDatabase.localAdd(filename, identity, url, new_commit_id, branch);

    // Save the downloaded data under the generated file name
    QFile file(saveFileAs);
    file.open(QIODevice::WriteOnly);
    file.write(device->readAll());

    // Set last modified data of the new file to the one provided by the server
    // Before version 5.10, Qt didn't offer any option to set this attribute, so we're not setting it at the moment
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    file.setFileTime(last_modified, QFileDevice::FileModificationTime);
#endif

    // Close file
    file.close();

    // Update info on currently opened file
    currently_opened_file_info = remoteDatabase.localGetLocalFileInfo(saveFileAs);

    // Refresh data
    refreshLocalFileList();

    // Tell the application to open this file
    emit openFile(saveFileAs);
}
