/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "digitallyimportedservicebase.h"
#include "icecastservice.h"
#include "jamendoservice.h"
#include "magnatuneservice.h"
#include "internetmimedata.h"
#include "internetmodel.h"
#include "internetservice.h"
#include "savedradio.h"
#include "somafmservice.h"
#include "groovesharkservice.h"
#include "subsonicservice.h"
#include "core/logging.h"
#include "core/mergedproxymodel.h"
#include "smartplaylists/generatormimedata.h"

#ifdef HAVE_LIBLASTFM
  #include "lastfmservice.h"
#endif
#ifdef HAVE_SPOTIFY
  #include "spotifyservice.h"
#endif

#include <QMimeData>
#include <QtDebug>

using smart_playlists::Generator;
using smart_playlists::GeneratorMimeData;
using smart_playlists::GeneratorPtr;

QMap<QString, InternetService*>* InternetModel::sServices = NULL;

InternetModel::InternetModel(BackgroundThread<Database>* db_thread,
                       TaskManager* task_manager, PlayerInterface* player,
                       CoverProviders* cover_providers,
                       GlobalSearch* global_search, QObject* parent)
  : QStandardItemModel(parent),
    db_thread_(db_thread),
    merged_model_(new MergedProxyModel(this)),
    task_manager_(task_manager),
    player_(player),
    cover_providers_(cover_providers),
    global_search_(global_search)
{
  if (!sServices) {
    sServices = new QMap<QString, InternetService*>;
  }
  Q_ASSERT(sServices->isEmpty());

  merged_model_->setSourceModel(this);

  AddService(new DigitallyImportedService(this));
  AddService(new IcecastService(this));
  AddService(new JamendoService(this));
#ifdef HAVE_LIBLASTFM
  AddService(new LastFMService(this));
#endif
  AddService(new GroovesharkService(this));
  AddService(new MagnatuneService(this));
  AddService(new SavedRadio(this));
  AddService(new SkyFmService(this));
  AddService(new SomaFMService(this));
#ifdef HAVE_SPOTIFY
  AddService(new SpotifyService(this));
#endif
  AddService(new SubsonicService(this));
}

void InternetModel::AddService(InternetService *service) {
  QStandardItem* root = service->CreateRootItem();
  if (!root) {
    qLog(Warning) << "Internet service" << service->name() << "did not return a root item";
    return;
  }

  root->setData(Type_Service, Role_Type);
  root->setData(QVariant::fromValue(service), Role_Service);

  invisibleRootItem()->appendRow(root);
  qLog(Debug) << "Adding internet service:" << service->name();
  sServices->insert(service->name(), service);

  connect(service, SIGNAL(StreamError(QString)), SIGNAL(StreamError(QString)));
  connect(service, SIGNAL(StreamMetadataFound(QUrl,Song)), SIGNAL(StreamMetadataFound(QUrl,Song)));
  connect(service, SIGNAL(OpenSettingsAtPage(SettingsDialog::Page)), SIGNAL(OpenSettingsAtPage(SettingsDialog::Page)));
  connect(service, SIGNAL(AddToPlaylistSignal(QMimeData*)), SIGNAL(AddToPlaylist(QMimeData*)));
  connect(service, SIGNAL(destroyed()), SLOT(ServiceDeleted()));

  service->ReloadSettings();
}

void InternetModel::RemoveService(InternetService* service) {
  if (!sServices->contains(service->name()))
    return;

  // Find and remove the root item that this service created
  for (int i=0 ; i<invisibleRootItem()->rowCount() ; ++i) {
    QStandardItem* item = invisibleRootItem()->child(i);
    if (!item || item->data(Role_Service).value<InternetService*>() == service) {
      invisibleRootItem()->removeRow(i);
      break;
    }
  }

  // Remove the service from the list
  sServices->remove(service->name());

  // Disconnect the service
  disconnect(service, 0, this, 0);
}

void InternetModel::ServiceDeleted() {
  InternetService* service = qobject_cast<InternetService*>(sender());
  if (service)
    RemoveService(service);
}

InternetService* InternetModel::ServiceByName(const QString& name) {
  if (sServices->contains(name))
    return sServices->value(name);
  return NULL;
}

InternetService* InternetModel::ServiceForItem(const QStandardItem* item) const {
  return ServiceForIndex(indexFromItem(item));
}

InternetService* InternetModel::ServiceForIndex(const QModelIndex& index) const {
  QModelIndex current_index = index;
  while (current_index.isValid()) {
    InternetService* service = current_index.data(Role_Service).value<InternetService*>();
    if (service) {
      return service;
    }
    current_index = current_index.parent();
  }
  return NULL;
}

Qt::ItemFlags InternetModel::flags(const QModelIndex& index) const {
  Qt::ItemFlags flags = Qt::ItemIsSelectable |
                        Qt::ItemIsEnabled |
                        Qt::ItemIsDropEnabled;
  if (IsPlayable(index)) {
    flags |= Qt::ItemIsDragEnabled;
  }
  return flags;
}

bool InternetModel::hasChildren(const QModelIndex& parent) const {
  if (parent.data(Role_CanLazyLoad).toBool())
    return true;
  return QStandardItemModel::hasChildren(parent);
}

int InternetModel::rowCount(const QModelIndex& parent) const {
  if (parent.data(Role_CanLazyLoad).toBool()) {
    QStandardItem* item = itemFromIndex(parent);
    InternetService* service = ServiceForItem(item);
    if (service) {
      item->setData(false, Role_CanLazyLoad);
      service->LazyPopulate(item);
    }
  }

  return QStandardItemModel::rowCount(parent);
}

bool InternetModel::IsPlayable(const QModelIndex& index) const {
  QVariant behaviour = index.data(Role_PlayBehaviour);
  if (!behaviour.isValid())
    return false;

  PlayBehaviour pb = PlayBehaviour(behaviour.toInt());
  return (pb == PlayBehaviour_SingleItem || PlayBehaviour_UseSongLoader);
}

QStringList InternetModel::mimeTypes() const {
  return QStringList() << "text/uri-list";
}

QMimeData* InternetModel::mimeData(const QModelIndexList& indexes) const {
  // Special case for when the user double clicked on a special item.
  if (indexes.count() == 1 &&
      indexes[0].data(Role_PlayBehaviour).toInt() ==
          PlayBehaviour_DoubleClickAction) {
    InternetModel::ServiceForIndex(indexes[0])->ItemDoubleClicked(itemFromIndex(indexes[0]));
    return NULL;
  }

  if (indexes.count() == 1 &&
      indexes[0].data(Role_Type).toInt() == Type_SmartPlaylist) {
    GeneratorPtr generator =
        InternetModel::ServiceForIndex(indexes[0])->CreateGenerator(itemFromIndex(indexes[0]));
    if (!generator)
      return NULL;
    GeneratorMimeData* data = new GeneratorMimeData(generator);
    data->setData(LibraryModel::kSmartPlaylistsMimeType, QByteArray());
    data->name_for_new_playlist_ = this->data(indexes.first()).toString();
    return data;
  }

  QList<QUrl> urls;
  QModelIndexList new_indexes;

  QModelIndex last_valid_index;
  foreach (const QModelIndex& index, indexes) {
    if (!IsPlayable(index))
      continue;

    last_valid_index = index;
    if (index.data(Role_Type).toInt() == Type_UserPlaylist) {
      // Get children
      int row = 0;
      int column = 0;
      QModelIndex child = index.child(row, column);
      while (child.isValid()) {
        new_indexes << child;
        urls << child.data(Role_Url).toUrl();
        child = index.child(++row, column);
      }
    } else {
      new_indexes = indexes;
      urls << index.data(Role_Url).toUrl();
    }
  }

  if (urls.isEmpty())
    return NULL;

  InternetMimeData* data = new InternetMimeData(this);
  data->setUrls(urls);
  data->indexes = new_indexes;
  data->name_for_new_playlist_ = InternetModel::ServiceForIndex(last_valid_index)->name();

  return data;
}

bool InternetModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) {
  if (action == Qt::IgnoreAction) {
    return false;
  }
  if (parent.data(Role_CanBeModified).toBool()) {
    InternetModel::ServiceForIndex(parent)->DropMimeData(data, parent);
  }

  return true;
}

void InternetModel::ShowContextMenu(const QModelIndex& merged_model_index,
                                 const QPoint& global_pos) {
  InternetService* service = ServiceForIndex(merged_model_index);
  if (service)
    service->ShowContextMenu(merged_model_->mapToSource(merged_model_index), global_pos);
}

void InternetModel::ReloadSettings() {
  foreach (InternetService* service, sServices->values()) {
    service->ReloadSettings();
  }
}
