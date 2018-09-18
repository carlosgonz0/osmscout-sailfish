/*
  OSMScout for SFOS
  Copyright (C) 2018 Lukas Karas

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "CollectionMapBridge.h"

CollectionMapBridge::CollectionMapBridge(QObject *parent):
  QObject(parent)
{
  Storage *storage = Storage::getInstance();
  if (storage) {
    connect(storage, SIGNAL(initialised()),
            this, SLOT(init()),
            Qt::QueuedConnection);

    connect(storage, SIGNAL(initialisationError(QString)),
            this, SLOT(storageInitialisationError(QString)),
            Qt::QueuedConnection);

    connect(this, SIGNAL(collectionLoadRequest()),
            storage, SLOT(loadCollections()),
            Qt::QueuedConnection);

    connect(storage, SIGNAL(collectionsLoaded(std::vector<Collection>, bool)),
            this, SLOT(onCollectionsLoaded(std::vector<Collection>, bool)),
            Qt::QueuedConnection);

    connect(storage, SIGNAL(error(QString)),
            this, SIGNAL(error(QString)),
            Qt::QueuedConnection);

    connect(this, SIGNAL(collectionDetailRequest(Collection)),
            storage, SLOT(loadCollectionDetails(Collection)),
            Qt::QueuedConnection);

    connect(storage, SIGNAL(collectionDetailsLoaded(Collection, bool)),
            this, SLOT(onCollectionDetailsLoaded(Collection, bool)),
            Qt::QueuedConnection);

    connect(this, SIGNAL(trackDataRequest(Track)),
            storage, SLOT(loadTrackData(Track)),
            Qt::QueuedConnection);

    connect(storage, SIGNAL(trackDataLoaded(Track, bool, bool)),
            this, SLOT(onTrackDataLoaded(Track, bool, bool)),
            Qt::QueuedConnection);

    init();
  }
}

void CollectionMapBridge::init()
{
  if (delegatedMap == nullptr){
    return;
  }

  emit collectionLoadRequest();
}

void CollectionMapBridge::storageInitialisationError(QString e)
{
  emit error(e);
}

void CollectionMapBridge::onCollectionDetailsLoaded(Collection collection, bool /*ok*/)
{
  if (!collection.visible || delegatedMap == nullptr){
    return;
  }

  qDebug() << "Display collection" << collection.name << "(" << collection.id << ")";

  QSet<qint64> wptToHide = displayedWaypoints[collection.id];
  QSet<qint64> trkToHide = displayedTracks[collection.id];
  QSet<qint64> &wptVisible = displayedWaypoints[collection.id];
  QSet<qint64> &trkVisible = displayedTracks[collection.id];
  wptVisible.clear();
  trkVisible.clear();

  if (collection.tracks){
    for (const auto &trk: *(collection.tracks)){
      emit trackDataRequest(trk);

      trkVisible.insert(trk.id);
      trkToHide.remove(trk.id);
    }
  }

  if (collection.waypoints){
    for (const auto &wpt: *(collection.waypoints)){
      qDebug() << "Adding overlay waypoint" <<
               QString::fromStdString(wpt.data.name.getOrElse("<empty>")) <<
               "(" << wpt.id << ")";

      osmscout::OverlayNode wptOverlay;
      wptOverlay.setTypeName(waypointTypeName);
      wptOverlay.addPoint(wpt.data.coord.GetLat(), wpt.data.coord.GetLon());
      wptOverlay.setName(QString::fromStdString(wpt.data.name.getOrElse("")));
      delegatedMap->addOverlayObject(overlayWptIdBase + wpt.id, &wptOverlay);

      wptVisible.insert(wpt.id);
      wptToHide.remove(wpt.id);
    }
  }

  for (const auto &id :wptToHide){
    delegatedMap->removeOverlayObject(id + overlayWptIdBase);
  }
  for (const auto &id :trkToHide){
    delegatedMap->removeOverlayObject(id + overlayTrkIdBase);
  }
}

void CollectionMapBridge::onTrackDataLoaded(Track track, bool complete, bool ok)
{
  if (delegatedMap == nullptr ||
      !complete ||
      !ok ||
      !displayedTracks.contains(track.collectionId)){
    return;
  }

  qDebug() << "Adding overlay track" <<
           track.name <<
           "(" << track.id << ")";

  for (const osmscout::gpx::TrackSegment &seg : track.data->segments) {
    std::vector<osmscout::Point> points;
    points.reserve(seg.points.size());
    for (auto const &p:seg.points) {
      points.emplace_back(0, p.coord);
    }
    osmscout::OverlayWay trkOverlay(points);
    trkOverlay.setTypeName(trackTypeName);
    trkOverlay.setName(track.name);
    delegatedMap->addOverlayObject(overlayTrkIdBase + track.id, &trkOverlay);
  }
}

void CollectionMapBridge::onCollectionsLoaded(std::vector<Collection> collections, bool /*ok*/)
{
  qDebug() << "Loaded" << collections.size() << "collections";

  // clear map
  if (delegatedMap!=nullptr) {
    for (const auto &wptSet: displayedWaypoints) {
      for (const auto &wptId: wptSet) {
        delegatedMap->removeOverlayObject(overlayWptIdBase + wptId);
      }
    }
    for (const auto &trkSet: displayedTracks) {
      for (const auto &trkId: trkSet) {
        delegatedMap->removeOverlayObject(overlayTrkIdBase + trkId);
      }
    }
  }
  displayedTracks.clear();
  displayedWaypoints.clear();

  for (const auto &c: collections){
    if (c.visible){
      collectionDetailRequest(c);
    }
  }
}

void CollectionMapBridge::setMap(QObject *map)
{
  delegatedMap = dynamic_cast<osmscout::MapWidget*>(map);
  if (delegatedMap == nullptr){
    return;
  }
  qDebug() << "CollectionMapBridge map:" << delegatedMap;
  init();
}

void CollectionMapBridge::setWaypointType(QString name)
{
  waypointTypeName = name;
  init();
}

void CollectionMapBridge::setTrackType(QString type)
{
  trackTypeName = type;
  init();
}