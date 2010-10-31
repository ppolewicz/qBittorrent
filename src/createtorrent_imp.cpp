/*
 * Bittorrent Client using Qt4 and libtorrent.
 * Copyright (C) 2006  Christophe Dumez
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 *
 * Contact : chris@qbittorrent.org
 */

#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/bind.hpp>

#include <libtorrent/version.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/file.hpp>
#include <libtorrent/storage.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/file_pool.hpp>
#include <libtorrent/create_torrent.hpp>

#include "torrentpersistentdata.h"
#include "createtorrent_imp.h"
#include "misc.h"
#include "qinisettings.h"

using namespace libtorrent;
using namespace boost::filesystem;

// do not include files and folders whose
// name starts with a .
bool file_filter(boost::filesystem::path const& filename)
{
  if (filename.leaf()[0] == '.') return false;
  std::cerr << filename << std::endl;
  return true;
}

createtorrent::createtorrent(QWidget *parent): QDialog(parent){
  setupUi(this);
  setAttribute(Qt::WA_DeleteOnClose);
  setModal(true);
  creatorThread = new torrentCreatorThread(this);
  connect(creatorThread, SIGNAL(creationSuccess(QString, QString)), this, SLOT(handleCreationSuccess(QString, QString)));
  connect(creatorThread, SIGNAL(creationFailure(QString)), this, SLOT(handleCreationFailure(QString)));
  connect(creatorThread, SIGNAL(updateProgress(int)), this, SLOT(updateProgressBar(int)));
  path::default_name_check(no_check);
  show();
}

createtorrent::~createtorrent() {
  delete creatorThread;
}

void createtorrent::on_addFolder_button_clicked(){
  QIniSettings settings("qBittorrent", "qBittorrent");
  QString last_path = settings.value("CreateTorrent/last_add_path", QDir::homePath()).toString();
  QString dir = QFileDialog::getExistingDirectory(this, tr("Select a folder to add to the torrent"), last_path, QFileDialog::ShowDirsOnly);
  if(!dir.isEmpty()) {
    settings.setValue("CreateTorrent/last_add_path", dir);
#if defined(Q_WS_WIN) || defined(Q_OS_OS2)
    dir = dir.replace("/", "\\");
#endif
    textInputPath->setText(dir);
  }
}

void createtorrent::on_addFile_button_clicked(){
  QIniSettings settings("qBittorrent", "qBittorrent");
  QString last_path = settings.value("CreateTorrent/last_add_path", QDir::homePath()).toString();
  QString file = QFileDialog::getOpenFileName(this, tr("Select a file to add to the torrent"), last_path);
  if(!file.isEmpty()) {
    settings.setValue("CreateTorrent/last_add_path", misc::removeLastPathPart(file));
#if defined(Q_WS_WIN) || defined(Q_OS_OS2)
    file = file.replace("/", "\\");
#endif
    textInputPath->setText(file);
  }
}

void createtorrent::on_removeTracker_button_clicked() {
  QModelIndexList selectedIndexes = trackers_list->selectionModel()->selectedIndexes();
  for(int i=selectedIndexes.size()-1; i>=0; --i){
    QListWidgetItem *item = trackers_list->takeItem(selectedIndexes.at(i).row());
    delete item;
  }
}

int createtorrent::getPieceSize() const {
  switch(comboPieceSize->currentIndex()) {
  case 0:
    return 32*1024;
  case 1:
    return 64*1024;
  case 2:
    return 128*1024;
  case 3:
    return 256*1024;
  case 4:
    return 512*1024;
  case 5:
    return 1024*1024;
  case 6:
    return 2048*1024;
  default:
    return 256*1024;
  }
}

void createtorrent::on_addTracker_button_clicked() {
  bool ok;
  QString URL = QInputDialog::getText(this, tr("Please type an announce URL"),
                                      tr("Announce URL:", "Tracker URL"), QLineEdit::Normal,
                                      "http://", &ok);
  if(ok){
    if(trackers_list->findItems(URL, Qt::MatchFixedString).size() == 0)
      trackers_list->addItem(URL);
  }
}

void createtorrent::on_removeURLSeed_button_clicked(){
  QModelIndexList selectedIndexes = URLSeeds_list->selectionModel()->selectedIndexes();
  for(int i=selectedIndexes.size()-1; i>=0; --i){
    QListWidgetItem *item = URLSeeds_list->takeItem(selectedIndexes.at(i).row());
    delete item;
  }
}

void createtorrent::on_addURLSeed_button_clicked(){
  bool ok;
  QString URL = QInputDialog::getText(this, tr("Please type a web seed url"),
                                      tr("Web seed URL:"), QLineEdit::Normal,
                                      "http://", &ok);
  if(ok){
    if(URLSeeds_list->findItems(URL, Qt::MatchFixedString).size() == 0)
      URLSeeds_list->addItem(URL);
  }
}

QStringList createtorrent::allItems(QListWidget *list){
  QStringList res;
  unsigned int nbItems = list->count();
  for(unsigned int i=0; i< nbItems; ++i){
    res << list->item(i)->text();
  }
  return res;
}

// Main function that create a .torrent file
void createtorrent::on_createButton_clicked(){
  QString input = textInputPath->text().trimmed();
  if (input.endsWith(QDir::separator()))
    input.chop(1);
  if(input.isEmpty()){
    QMessageBox::critical(0, tr("No input path set"), tr("Please type an input path first"));
    return;
  }
  QStringList trackers = allItems(trackers_list);

  QIniSettings settings("qBittorrent", "qBittorrent");
  QString last_path = settings.value("CreateTorrent/last_save_path", QDir::homePath()).toString();

  QString destination = QFileDialog::getSaveFileName(this, tr("Select destination torrent file"), last_path, tr("Torrent Files")+QString::fromUtf8(" (*.torrent)"));
  if(!destination.isEmpty()) {
    settings.setValue("CreateTorrent/last_save_path", misc::removeLastPathPart(destination));
    if(!destination.toUpper().endsWith(".TORRENT"))
      destination += QString::fromUtf8(".torrent");
  } else {
    return;
  }
  // Disable dialog
  setEnabled(false);
  // Set busy cursor
  setCursor(QCursor(Qt::WaitCursor));
  // Actually create the torrent
  QStringList url_seeds = allItems(URLSeeds_list);
  QString comment = txt_comment->toPlainText();
  creatorThread->create(input, destination, trackers, url_seeds, comment, check_private->isChecked(), getPieceSize());
}

void createtorrent::handleCreationFailure(QString msg) {
  // Enable dialog
  setEnabled(true);
  // Remove busy cursor
  setCursor(QCursor(Qt::ArrowCursor));
  QMessageBox::information(0, tr("Torrent creation"), tr("Torrent creation was unsuccessful, reason: %1").arg(msg));
}

void createtorrent::handleCreationSuccess(QString path, QString branch_path) {
  // Enable Dialog
  setEnabled(true);
  // Remove busy cursor
  setCursor(QCursor(Qt::ArrowCursor));
  if(checkStartSeeding->isChecked()) {
    QString root_folder;
    // Create save path temp data
    boost::intrusive_ptr<torrent_info> t;
    try {
      t = new torrent_info(path.toUtf8().data());
      root_folder = misc::truncateRootFolder(t);
    } catch(std::exception&) {
      QMessageBox::critical(0, tr("Torrent creation"), tr("Created torrent file is invalid. It won't be added to download list."));
      return;
    }
    QString hash = misc::toQString(t->info_hash());
    QString save_path = branch_path;
    if(!root_folder.isEmpty()) {
      save_path = QDir(save_path).absoluteFilePath(root_folder);
    }
    TorrentTempData::setSavePath(hash, save_path);
#if LIBTORRENT_VERSION_MINOR > 14
    // Enable seeding mode (do not recheck the files)
    TorrentTempData::setSeedingMode(hash, true);
#endif
    emit torrent_to_seed(path);
  }
  QMessageBox::information(0, tr("Torrent creation"), tr("Torrent was created successfully:")+" "+path);
  close();
}

void createtorrent::on_cancelButton_clicked() {
  // End torrent creation thread
  if(creatorThread->isRunning()) {
    creatorThread->abortCreation();
    creatorThread->terminate();
    // Wait for termination
    creatorThread->wait();
  }
  // Close the dialog
  reject();
}

void createtorrent::updateProgressBar(int progress) {
  progressBar->setValue(progress);
}

//
// Torrent Creator Thread
//

void torrentCreatorThread::create(QString _input_path, QString _save_path, QStringList _trackers, QStringList _url_seeds, QString _comment, bool _is_private, int _piece_size) {
  input_path = _input_path;
  save_path = _save_path;
  trackers = _trackers;
  url_seeds = _url_seeds;
  comment = _comment;
  is_private = _is_private;
  piece_size = _piece_size;
  abort = false;
  start();
}

void sendProgressUpdateSignal(int i, int num, torrentCreatorThread *parent){
  parent->sendProgressSignal((int)(i*100./(float)num));
}

void torrentCreatorThread::sendProgressSignal(int progress) {
  emit updateProgress(progress);
}

void torrentCreatorThread::run() {
  emit updateProgress(0);
  char const* creator_str = "qBittorrent "VERSION;
  try {
    file_storage fs;
    path full_path = complete(path(input_path.toUtf8().constData()));
    // Adding files to the torrent
    add_files(fs, full_path, file_filter);
    if(abort) return;
    create_torrent t(fs, piece_size);

    // Add url seeds
    QString seed;
    foreach(seed, url_seeds){
      t.add_url_seed(seed.toLocal8Bit().data());
    }
    for(int i=0; i<trackers.size(); ++i){
      t.add_tracker(trackers.at(i).toLocal8Bit().data());
    }
    if(abort) return;
    // calculate the hash for all pieces
    set_piece_hashes(t, full_path.branch_path(), boost::bind<void>(&sendProgressUpdateSignal, _1, t.num_pieces(), this));
    // Set qBittorrent as creator and add user comment to
    // torrent_info structure
    t.set_creator(creator_str);
    t.set_comment((const char*)comment.toUtf8());
    // Is private ?
    t.set_priv(is_private);
    if(abort) return;
    // create the torrent and print it to out
    ofstream out(complete(path((const char*)save_path.toUtf8())), std::ios_base::binary);
    bencode(std::ostream_iterator<char>(out), t.generate());
    emit updateProgress(100);
    emit creationSuccess(save_path, QString::fromUtf8(full_path.branch_path().string().c_str()));
  }
  catch (std::exception& e){
    emit creationFailure(QString::fromUtf8(e.what()));
  }
}