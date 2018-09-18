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

#include "Storage.h"
#include "QVariantConverters.h"

#include <osmscout/OSMScoutQt.h>
#include <osmscout/gpx/GpxFile.h>
#include <osmscout/gpx/Import.h>

#include <QDebug>
#include <QThread>
#include <QtSql/QSqlQuery>
#include <osmscout/gpx/Export.h>

namespace {
  static constexpr int DbSchema = 1;
  static constexpr int TrackPointBatchSize = 10000;
  static constexpr int WayPointBatchSize = 100;
}

using namespace osmscout;
using namespace converters;

static Storage* storage = nullptr;

void ErrorCallback::Error(std::string err)
{
  gpx::ProcessCallback::Error(err);
  emit error(QString::fromStdString(err));
}

Storage::Storage(QThread *thread,
                 const QDir &directory)
  :thread(thread),
   directory(directory)
{
}

Storage::~Storage()
{
  if (thread != QThread::currentThread()){
    qWarning() << "Destroy" << this << "from non incorrect thread;" << thread << "!=" << QThread::currentThread();
  }
  if (thread != nullptr){
    thread->quit();
  }

  if (db.isValid()) {
    if (db.isOpen()) {
      db.close();
    }
    db = QSqlDatabase(); // invalidate instance
    QSqlDatabase::removeDatabase("storage");
  }
  qDebug() << "Storage is closed";
}

bool Storage::updateSchema(){
  int currentSchema=0;
  QStringList tables = db.tables();

  if (tables.contains("version")){
    QString sql("SELECT MAX(`version`) AS `currentVersion` FROM `version`;");

    QSqlQuery q = db.exec(sql);
    if (!q.lastError().isValid()){
      if (q.next()){
        QVariant val = q.value(0);
        if (!val.isNull()){
          bool ok = false;
          currentSchema = val.toInt(&ok);
          if (!ok) {
            qWarning() << "Loading currentSchema value failed" << val;
          }
          qDebug()<<"last schema: "<<currentSchema;
        }
      } else {
        qWarning() << "Loading currentSchema value failed (no entry)";
      }
    }else{
      qWarning() << "failed to get schema version " << q.lastError();
    }
  }

  if (!tables.contains("version")){
    QString sql("CREATE TABLE `version` ( `version` int NOT NULL);");
    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()){
      qWarning() << "Creating version table failed" << q.lastError();
      db.close();
      return false;
    }

    QSqlQuery sqlInsert(db);
    sqlInsert.prepare("INSERT INTO `version` (`version`) VALUES (:version);");
    sqlInsert.bindValue(":version", DbSchema);
    sqlInsert.exec();
    if (sqlInsert.lastError().isValid()){
      qWarning() << "Creating version table entry failed" << sqlInsert.lastError();
      db.close();
      return false;
    }
    currentSchema = DbSchema;
  }
  if (currentSchema > DbSchema) {
    qWarning() << "newer database schema; " << currentSchema << " > " << DbSchema;
  }

  if (!tables.contains("collection")){
    qDebug()<< "creating collection table";

    QString sql("CREATE TABLE `collection`");
    sql.append("(").append( "`id` INTEGER PRIMARY KEY");
    sql.append(",").append( "`name` varchar(255) NOT NULL ");
    sql.append(",").append( "`description` varchar(255) NULL ");
    sql.append(",").append( "`visible` tinyint(1) NOT NULL");
    sql.append(");");

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()){
      qWarning() << "Storage: creating collection table failed" << q.lastError();
      db.close();
      return false;
    }
  }

  if (!tables.contains("track")){
    qDebug()<< "creating track table";

    QString sql("CREATE TABLE `track`");
    sql.append("(").append( "`id` INTEGER PRIMARY KEY");
    sql.append(",").append( "`collection_id` INTEGER NOT NULL REFERENCES collection(id) ON DELETE CASCADE");
    sql.append(",").append( "`name` varchar(255) NOT NULL");
    sql.append(",").append( "`description` varchar(255) NULL");
    sql.append(",").append( "`open` tinyint(1) NOT NULL");
    sql.append(",").append( "`creation_time` datetime NOT NULL");
    sql.append(",").append( "`distance` double NOT NULL");

    // bbox
    sql.append(",").append( "`bbox_min_lat` double NOT NULL");
    sql.append(",").append( "`bbox_min_lon` double NOT NULL");
    sql.append(",").append( "`bbox_max_lat` double NOT NULL");
    sql.append(",").append( "`bbox_max_lon` double NOT NULL");

    sql.append(");");

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()){
      qWarning() << "Storage: creating track table failed" << q.lastError();
      db.close();
      return false;
    }
  }

  if (!tables.contains("track_segment")){
    qDebug()<< "creating track_segment table";

    QString sql("CREATE TABLE `track_segment`");
    sql.append("(").append( "`id` INTEGER PRIMARY KEY");
    sql.append(",").append( "`track_id` INTEGER NOT NULL REFERENCES track(id) ON DELETE CASCADE");
    sql.append(",").append( "`open` tinyint(1) NOT NULL");
    sql.append(",").append( "`creation_time` datetime NOT NULL");
    sql.append(",").append( "`distance` double NOT NULL");
    sql.append(");");

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()){
      qWarning() << "Storage: creating track segment table failed" << q.lastError();
      db.close();
      return false;
    }
  }

  if (!tables.contains("track_point")){
    qDebug()<< "creating track_point table";

    QString sql("CREATE TABLE `track_point`");
    sql.append("(").append( "`segment_id` INTEGER NOT NULL REFERENCES track_segment(id) ON DELETE CASCADE");
    sql.append(",").append( "`timestamp` datetime NOT NULL");
    sql.append(",").append( "`latitude` double NOT NULL");
    sql.append(",").append( "`longitude` double NOT NULL");
    sql.append(",").append( "`elevation` double NULL ");
    sql.append(",").append( "`horiz_accuracy` double NULL ");
    sql.append(",").append( "`vert_accuracy` double NULL ");
    sql.append(");");

    // TODO: what satelites and compas?

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()){
      qWarning() << "Storage: creating track node table failed" << q.lastError();
      db.close();
      return false;
    }
  }

  if (!tables.contains("waypoint")){
    qDebug()<< "creating waypoints table";

    QString sql("CREATE TABLE `waypoint`");
    sql.append("(").append( "`id` INTEGER PRIMARY KEY");
    sql.append(",").append( "`collection_id` INTEGER NOT NULL REFERENCES collection(id) ON DELETE CASCADE");
    sql.append(",").append( "`timestamp` datetime NOT NULL");
    sql.append(",").append( "`latitude` double NOT NULL");
    sql.append(",").append( "`longitude` double NOT NULL");
    sql.append(",").append( "`elevation` double NULL");
    sql.append(",").append( "`name` varchar(255) NOT NULL ");
    sql.append(",").append( "`description` varchar(255) NULL ");
    sql.append(",").append( "`symbol` varchar(255) NULL ");
    sql.append(");");

    QSqlQuery q = db.exec(sql);
    if (q.lastError().isValid()){
      qWarning() << "Storage: creating waypoints table failed" << q.lastError();
      db.close();
      return false;
    }
  }

  return true;
}

void Storage::init()
{
  if (!checkAccess("init", false)){
    return;
  }

  // Find QSLite driver
  db = QSqlDatabase::addDatabase("QSQLITE", "storage");
  if (!db.isValid()){
    qWarning() << "Could not find QSQLITE backend";
    emit initialisationError("Could not find QSQLITE backend");
    return;
  }
  QString path=directory.path();
  if (!directory.mkpath(path)){
    qWarning() << "Failed to cretate directory" << directory;
    emit initialisationError(QString("Failed to cretate directory ") + directory.path());
    return;
  }
  path.append(QDir::separator()).append("storage.db");
  path = QDir::toNativeSeparators(path);
  db.setDatabaseName(path);

  if (!db.open()){
    qWarning() << "Open database failed" << db.lastError();
    emit initialisationError(db.lastError().text());
    return;
  }
  qDebug() << "Storage database opened:" << path;
  if (!updateSchema()){
    emit initialisationError("update schema");
    return;
  }

  QSqlQuery q = db.exec("PRAGMA foreign_keys = ON;");
  if (q.lastError().isValid()){
    qWarning() << "Enabling foreign keys fails:" << q.lastError();
  }

  ok = db.isValid() && db.isOpen();
  emit initialised();
}

bool Storage::checkAccess(QString slotName, bool requireOpen)
{
  if (thread != QThread::currentThread()){
    qWarning() << this << "::" << slotName << "from non incorrect thread;" << thread << "!=" << QThread::currentThread();
    return false;
  }
  if (requireOpen && !ok){
    qWarning() << "Database is not open, " << this << "::" << slotName << "";
    return false;
  }
  return true;
}

void Storage::loadCollections()
{
  if (!checkAccess("loadCollections")){
    emit collectionsLoaded(std::vector<Collection>(), false);
    return;
  }

  QString sql("SELECT `id`, `visible`, `name`, `description` FROM `collection`;");

  QSqlQuery q = db.exec(sql);
  if (q.lastError().isValid()) {
    emit collectionsLoaded(std::vector<Collection>(), false);
  }
  std::vector<Collection> result;
  while (q.next()) {
    result.emplace_back(
      varToLong(q.value("id")),
      varToBool(q.value("visible")),
      varToString(q.value("name")),
      varToString(q.value("description"))
    );
  }
  emit collectionsLoaded(result, true);
}

Track Storage::makeTrack(QSqlQuery &sqlTrack) const
{
  return Track(varToLong(sqlTrack.value("id")),
               varToLong(sqlTrack.value("collection_id")),
               varToString(sqlTrack.value("name")),
               varToString(sqlTrack.value("description")),
               varToBool(sqlTrack.value("open")),
               varToDateTime(sqlTrack.value("creation_time")),
               osmscout::Distance::Of<osmscout::Meter>(varToDouble(sqlTrack.value("distance"))),
               osmscout::GeoBox(osmscout::GeoCoord(varToDouble(sqlTrack.value("bbox_min_lat")),
                                                   varToDouble(sqlTrack.value("bbox_min_lon"))),
                                osmscout::GeoCoord(varToDouble(sqlTrack.value("bbox_max_lat")),
                                                   varToDouble(sqlTrack.value("bbox_max_lon")))));
}

std::shared_ptr<std::vector<Track>> Storage::loadTracks(qint64 collectionId)
{
  QSqlQuery sqlTrack(db);
  sqlTrack.prepare("SELECT * FROM `track` WHERE collection_id = :collectionId;");
  sqlTrack.bindValue(":collectionId", collectionId);
  sqlTrack.exec();

  if (sqlTrack.lastError().isValid()) {
    qWarning() << "Loading tracks for collection id" << collectionId << "fails";
    emit error(tr("Loading tracks for collection id %1 fails").arg(collectionId));
    return nullptr;
  }

  std::shared_ptr<std::vector<Track>> result = std::make_shared<std::vector<Track>>();
  while (sqlTrack.next()) {
    result->emplace_back(makeTrack(sqlTrack));
  }
  return result;
}

std::shared_ptr<std::vector<Waypoint>> Storage::loadWaypoints(qint64 collectionId)
{
  QSqlQuery sql(db);
  sql.prepare("SELECT `id`, `name`, `description`, `symbol`, `timestamp`, `latitude`, `longitude`, `elevation` FROM `waypoint` WHERE collection_id = :collectionId;");
  sql.bindValue(":collectionId", collectionId);
  sql.exec();

  if (sql.lastError().isValid()) {
    qWarning() << "Loading waypoints for collection id" << collectionId << "fails";
    emit error(tr("Loading waypoints for collection id %1 fails").arg(collectionId));
    return nullptr;
  }

  std::shared_ptr<std::vector<Waypoint>> result = std::make_shared<std::vector<Waypoint>>();
  while (sql.next()) {
    gpx::Waypoint wpt(osmscout::GeoCoord(
      varToDouble(sql.value("latitude")),
      varToDouble(sql.value("longitude"))
      ));

    wpt.name = varToStringOpt(varToString(sql.value("name")));
    wpt.description = varToStringOpt(sql.value("description"));
    wpt.symbol = varToStringOpt(sql.value("symbol"));

    wpt.time = gpx::Optional<Timestamp>::of(dateTimeToTimestamp(varToDateTime(sql.value("timestamp"))));
    wpt.elevation = varToDoubleOpt(sql.value("elevation"));

    result->emplace_back(varToLong(sql.value("id")), std::move(wpt));
  }
  return result;
}

bool Storage::loadCollectionDetailsPrivate(Collection &collection)
{
  QSqlQuery sql(db);
  sql.prepare("SELECT `name`, `description` FROM `collection` WHERE id = :collectionId;");
  sql.bindValue(":collectionId", collection.id);
  sql.exec();
  if (sql.lastError().isValid()) {
    qWarning() << "Loading collection id" << collection.id << "fails";
    emit error(tr("Loading collection id %1 fails").arg(collection.id));
    return false;
  }
  std::vector<Collection> result;
  if (!sql.next()) {
    qWarning() << "Collection id" << collection.id << "don't exists";
    emit error(tr("Collection id %1 don't exists").arg(collection.id));
    return false;
  }

  collection.name = varToString(sql.value("name"));
  collection.description = varToString(sql.value("description"));

  collection.tracks = loadTracks(collection.id);
  collection.waypoints = loadWaypoints(collection.id);
  return true;
}

void Storage::loadCollectionDetails(Collection collection)
{
  if (!checkAccess("loadCollectionDetails")){
    emit collectionDetailsLoaded(collection, false);
    return;
  }

  if (loadCollectionDetailsPrivate(collection)) {
    emit collectionDetailsLoaded(collection, true);
  }else{
    emit collectionDetailsLoaded(collection, false);
  }
}

void Storage::loadTrackPoints(qint64 segmentId, gpx::TrackSegment &segment)
{
  QSqlQuery sql(db);
  sql.prepare("SELECT `timestamp`, `latitude`, `longitude`, `elevation`, `horiz_accuracy`, `vert_accuracy` FROM `track_point` WHERE segment_id = :segmentId;");
  sql.bindValue(":segmentId", segmentId);
  sql.exec();
  if (sql.lastError().isValid()) {
    qWarning() << "Loading nodes for segment id" << segmentId << "failed";
    emit error(tr("Loading nodes for segment id %1 failed: %2").arg(segmentId).arg(sql.lastError().text()));
    return;
  }

  while (sql.next()) {
    gpx::TrackPoint point(osmscout::GeoCoord(
      varToDouble(sql.value("latitude")),
      varToDouble(sql.value("longitude"))
    ));

    point.time = gpx::Optional<Timestamp>::of(dateTimeToTimestamp(varToDateTime(sql.value("timestamp"))));
    point.elevation = varToDoubleOpt(sql.value("elevation"));

    // see TrackPoint notes
    point.hdop = varToDoubleOpt(sql.value("horiz_accuracy"));
    point.vdop = varToDoubleOpt(sql.value("vert_accuracy"));

    segment.points.push_back(std::move(point));
  }
}

bool Storage::loadTrackDataPrivate(Track &track)
{
  QSqlQuery sqlTrack(db);
  sqlTrack.prepare("SELECT * FROM `track` WHERE id = :trackId;");
  sqlTrack.bindValue(":trackId", track.id);
  sqlTrack.exec();

  if (sqlTrack.lastError().isValid()) {
    qWarning() << "Loading track id" << track.id << "fails: " << sqlTrack.lastError();
    emit error(tr("Loading track id %1 fails").arg(track.id));
    return false;
  }

  if (!sqlTrack.next()) {
    qWarning() << "Track id" << track.id << "don't exists";
    emit error(tr("Track id %1 don't exists").arg(track.id));
    return false;
  }
  track = makeTrack(sqlTrack);

  emit trackDataLoaded(track, false, true);

  track.data = std::make_shared<gpx::Track>();

  track.data->name = gpx::Optional<std::string>::of(track.name.toStdString());
  track.data->desc = gpx::Optional<std::string>::of(track.description.toStdString());

  QSqlQuery sql(db);
  sql.prepare("SELECT `id` FROM `track_segment` WHERE track_id = :trackId;");
  sql.bindValue(":trackId", track.id);
  sql.exec();
  if (sql.lastError().isValid()) {
    qWarning() << "Loading segments for track id" << track.id << "failed";
    emit error(tr("Loading segments for track id %1 failed: %2").arg(track.id).arg(sql.lastError().text()));
  }else{
    while (sql.next()) {
      gpx::TrackSegment segment;
      long segmentId = varToLong(sql.value("id"));
      loadTrackPoints(segmentId, segment);
      track.data->segments.push_back(std::move(segment));
    }
  }
  return true;
}

void Storage::loadTrackData(Track track)
{
  if (!checkAccess("loadTrackData")){
    emit trackDataLoaded(track, true, false);
    return;
  }

  if (loadTrackDataPrivate(track)) {
    emit trackDataLoaded(track, true, true);
  }else{
    emit trackDataLoaded(track, true, false);
  }
}

void Storage::updateOrCreateCollection(Collection collection)
{
  if (!checkAccess("updateOrCreateCollection")){
    emit collectionsLoaded(std::vector<Collection>(), false);
    return;
  }

  QSqlQuery sql(db);
  if (collection.id < 0){
    sql.prepare(
      "INSERT INTO `collection` (`name`, `description`, `visible`) VALUES (:name, :description, :visible);");
    sql.bindValue(":name", collection.name);
    sql.bindValue(":description", collection.description);
    sql.bindValue(":visible", collection.visible);
  }else {
    sql.prepare(
      "UPDATE `collection` SET `name` = :name, `description` = :description, `visible` = :visible WHERE (`id` = :id);");
    sql.bindValue(":id", collection.id);
    sql.bindValue(":name", collection.name);
    sql.bindValue(":description", collection.description);
    sql.bindValue(":visible", collection.visible);
  }
  sql.exec();
  if (sql.lastError().isValid()){
    if (collection.id < 0) {
      qWarning() << "Creating collection failed: " << sql.lastError();
      emit error(tr("Creating collection failed: %1").arg(sql.lastError().text()));
    }else {
      qWarning() << "Updating collection failed: " << sql.lastError();
      emit error(tr("Updating collection failed: %1").arg(sql.lastError().text()));
    }
  }

  loadCollections();
}

void Storage::deleteCollection(qint64 id)
{
  if (!checkAccess("deleteCollection")){
    emit collectionsLoaded(std::vector<Collection>(), false);
    return;
  }

  QSqlQuery sql(db);
  sql.prepare(
    "DELETE FROM `collection` WHERE (`id` = :id)");
  sql.bindValue(":id", id);
  sql.exec();
  if (sql.lastError().isValid()){
    qWarning() << "Deleting collection failed: " << sql.lastError();
    emit error(tr("Deleting collection failed: %1").arg(sql.lastError().text()));
  }

  loadCollections();
}

bool Storage::importWaypoints(const osmscout::gpx::GpxFile &gpxFile, qint64 collectionId)
{
  int wptNum = 0;
  db.transaction();
  QSqlQuery sqlWpt(db);
  sqlWpt.prepare(
    "INSERT INTO `waypoint` (`collection_id`, `timestamp`, `latitude`, `longitude`, `elevation`, `name`, `description`, `symbol`) VALUES (:collection_id, :timestamp, :latitude, :longitude, :elevation, :name, :description, :symbol)");
  for (const auto &wpt: gpxFile.waypoints) {
    wptNum++;

    QString wptName = QString::fromStdString(wpt.name.getOrElse(""));
    if (wptName.isEmpty())
      wptName = tr("waypoint %1").arg(wptNum);

    sqlWpt.bindValue(":collection_id", collectionId);
    sqlWpt.bindValue(":timestamp",
                     wpt.time.hasValue() ? timestampToDateTime(wpt.time) : QDateTime::currentDateTime());
    sqlWpt.bindValue(":latitude", wpt.coord.GetLat());
    sqlWpt.bindValue(":longitude", wpt.coord.GetLon());
    sqlWpt.bindValue(":elevation", (wpt.elevation.hasValue() ? wpt.elevation.get() : QVariant()));
    sqlWpt.bindValue(":name", wptName);
    sqlWpt.bindValue(":description",
                     (wpt.description.hasValue() ? QString::fromStdString(wpt.description.get()) : QVariant()));
    sqlWpt.bindValue(":symbol", (wpt.symbol.hasValue() ? QString::fromStdString(wpt.symbol.get()) : QVariant()));

    sqlWpt.exec();
    if (sqlWpt.lastError().isValid()) {
      qWarning() << "Import of waypoints failed" << sqlWpt.lastError();
      emit error(tr("Import of waypoints failed: %1").arg(sqlWpt.lastError().text()));
      if (!db.rollback()) {
        qWarning() << "Transaction rollback failed" << db.lastError();
      }
      return false;
    }
    // commit batch WayPointBatchSize queries
    if (wptNum % WayPointBatchSize == 0) {
      if (!db.commit()) {
        emit error(tr("Transaction commit failed: %1").arg(db.lastError().text()));
        qWarning() << "Transaction commit failed" << db.lastError();
        return false;
      }
      db.transaction();
    }
  }
  if (!db.commit()) {
    emit error(tr("Transaction commit failed: %1").arg(db.lastError().text()));
    qWarning() << "Transaction commit failed" << db.lastError();
    return false;
  }
  return true;
}
TrackStatistics Storage::computeTrackStatistics(const osmscout::gpx::Track &trk) const
{
  GeoBox bbox;
  for (const auto &seg:trk.segments){
    for (const auto &p:seg.points){
      bbox.Include(GeoBox(p.coord, p.coord));
    }
  }
  return TrackStatistics(trk.GetLength(), bbox);
}

bool Storage::importTracks(const osmscout::gpx::GpxFile &gpxFile, qint64 collectionId)
{
  int trkNum = 0;
  QSqlQuery sqlTrk(db);
  QSqlQuery sqlSeg(db);
  sqlTrk.prepare(QString("INSERT INTO `track` (")
    .append("`collection_id`, `name`, `description`, `open`, `creation_time`, ")
    .append("`distance`, `bbox_min_lat`, `bbox_min_lon`, `bbox_max_lat`, `bbox_max_lon`")
    .append(") ")
    .append("VALUES (")
    .append(":collection_id, :name, :description, :open, :creation_time, ")
    .append(":distance, :bboxMinLat, :bboxMinLon, :bboxMaxLat, :bboxMaxLon")
    .append(")"));

  sqlSeg.prepare("INSERT INTO `track_segment` (`track_id`, `open`, `creation_time`, `distance`) VALUES (:track_id, :open, :creation_time, :distance)");
  for (const auto &trk: gpxFile.tracks){
    trkNum++;

    QString trackName = QString::fromStdString(trk.name.getOrElse(""));
    if (trackName.isEmpty())
      trackName = tr("track %1").arg(trkNum);

    sqlTrk.bindValue(":collection_id", collectionId);
    sqlTrk.bindValue(":name", trackName);
    sqlTrk.bindValue(":description",
                     (trk.desc.hasValue() ? QString::fromStdString(trk.desc.get()) : QVariant()));
    sqlTrk.bindValue(":open", false);

    // TODO: compute statistics (time, distance...)
    TrackStatistics stat = computeTrackStatistics(trk);
    sqlTrk.bindValue(":creation_time", QDateTime::currentDateTime());
    sqlTrk.bindValue(":distance", stat.distance.AsMeter());

    sqlTrk.bindValue(":bboxMinLat", stat.bbox.GetMinLat());
    sqlTrk.bindValue(":bboxMinLon", stat.bbox.GetMinLon());
    sqlTrk.bindValue(":bboxMaxLat", stat.bbox.GetMaxLat());
    sqlTrk.bindValue(":bboxMaxLon", stat.bbox.GetMaxLon());

    sqlTrk.exec();
    if (sqlTrk.lastError().isValid()) {
      qWarning() << "Import of tracks failed" << sqlTrk.lastError();
      emit error(tr("Import of tracks failed: %1").arg(sqlTrk.lastError().text()));
      return false;
    }

    qint64 trackId = varToLong(sqlTrk.lastInsertId());

    for (auto const &seg: trk.segments){
      sqlSeg.bindValue(":track_id", trackId);
      sqlSeg.bindValue(":open", false);

      // TODO: compute statistics (time, distance...)
      sqlSeg.bindValue(":creation_time", QDateTime::currentDateTime());
      sqlSeg.bindValue(":distance", seg.GetLength().AsMeter());
      sqlSeg.exec();
      if (sqlSeg.lastError().isValid()) {
        qWarning() << "Import of segments failed" << sqlSeg.lastError();
        emit error(tr("Import of segments failed: %1").arg(sqlSeg.lastError().text()));
        return false;
      }
      qint64 segmentId = varToLong(sqlSeg.lastInsertId());

      if (!importTrackPoints(seg.points, segmentId)){
        qWarning() << "Import of track points failed" << sqlSeg.lastError();
        emit error(tr("Import of track points failed: %1").arg(sqlSeg.lastError().text()));
        return false;
      }
      qDebug() << "Imported" << seg.points.size() << "points to segment" << segmentId << "for track" << trackId;
    }
    qDebug() << "Imported track " << trackId;
  }
  return true;
}

bool Storage::importTrackPoints(const std::vector<osmscout::gpx::TrackPoint> &points, qint64 segmentId)
{
  size_t pointNum = 0;
  db.transaction();
  QSqlQuery sql(db);
  sql.prepare("INSERT INTO `track_point` (`segment_id`, `timestamp`, `latitude`, `longitude`, `elevation`, `horiz_accuracy`, `vert_accuracy`) VALUES (:segment_id, :timestamp, :latitude, :longitude, :elevation, :horiz_accuracy, :vert_accuracy)");

  for (const auto &point: points){
    pointNum ++;

    if (!point.time.hasValue()){
      qWarning() << "Track point without timestammp";
      continue;
    }
    sql.bindValue(":segment_id", segmentId);
    sql.bindValue(":timestamp", timestampToDateTime(point.time));
    sql.bindValue(":latitude", point.coord.GetLat());
    sql.bindValue(":longitude", point.coord.GetLon());
    sql.bindValue(":elevation", point.elevation.hasValue() ? point.elevation.get() : QVariant());
    // read TrackPoint notes
    sql.bindValue(":horiz_accuracy", point.hdop.hasValue() ? point.hdop.get() : QVariant());
    sql.bindValue(":vert_accuracy", point.vdop.hasValue() ? point.vdop.get(): QVariant());

    sql.exec();
    if (sql.lastError().isValid()) {
      qWarning() << "Import of track points failed" << sql.lastError();
      emit error(tr("Import of track points failed: %1").arg(sql.lastError().text()));
      if (!db.rollback()) {
        qWarning() << "Transaction rollback failed" << db.lastError();
      }
      return false;
    }
    // commit batch TrackPointBatchSize queries
    if (pointNum % TrackPointBatchSize == 0) {
      if (!db.commit()) {
        emit error(tr("Transaction commit failed: %1").arg(db.lastError().text()));
        qWarning() << "Transaction commit failed" << db.lastError();
        return false;
      }
      db.transaction();
    }
  }
  if (!db.commit()) {
    emit error(tr("Transaction commit failed: %1").arg(db.lastError().text()));
    qWarning() << "Transaction commit failed" << db.lastError();
    return false;
  }
  return true;
}

void Storage::importCollection(QString filePath)
{
  if (!checkAccess("importCollection")){
    emit collectionsLoaded(std::vector<Collection>(), false);
    return;
  }

  QTime timer;
  timer.start();
  qDebug() << "Importing collection from" << filePath;

  gpx::GpxFile gpxFile;
  std::shared_ptr<ErrorCallback> callback = std::make_shared<ErrorCallback>();
  connect(callback.get(), SIGNAL(error(QString)), this, SIGNAL(error(QString)));

  if (!gpx::ImportGpx(filePath.toStdString(),
                      gpxFile,
                      nullptr,
                      std::static_pointer_cast<gpx::ProcessCallback, ErrorCallback>(callback))){

    qWarning() << "Gpx import failed " << filePath;
    loadCollections();
    return;
  }

  // import collection
  QSqlQuery sql(db);
  sql.prepare("INSERT INTO `collection` (`name`, `description`, `visible`) VALUES (:name, :description, 0);");
  sql.bindValue(":name", gpxFile.name.hasValue() ?
                         QString::fromStdString(gpxFile.name.get()) : QFileInfo(filePath).baseName());
  sql.bindValue(":description", gpxFile.desc.hasValue() ?
                                QString::fromStdString(gpxFile.desc.get()) :
                                tr("Imported from %1").arg(filePath));

  sql.exec();
  if (sql.lastError().isValid()){
    qWarning() << "Creating collection failed" << sql.lastError();
    emit error(tr("Creating collection failed: %1").arg(sql.lastError().text()));
    loadCollections();
    return;
  }
  qint64 collectionId = varToLong(sql.lastInsertId());
  if (collectionId < 0){
    qWarning() << "Invalid collection id" << collectionId;
    emit error(tr("Invalid collection id: %1").arg(collectionId));
    loadCollections();
    return;
  }

  // import waypoints
  if (!gpxFile.waypoints.empty()) {
    if (!importWaypoints(gpxFile, collectionId)){
      loadCollections();
      return;
    }
  }
  qDebug() << "Imported" << gpxFile.waypoints.size() << "waypoints to collection" << collectionId << "from" << filePath;

  // import tracks
  if (!gpxFile.tracks.empty()) {
    if (!importTracks(gpxFile, collectionId)){
      loadCollections();
      return;
    }
  }
  qDebug() << "Imported" << gpxFile.tracks.size() << "tracks to collection" << collectionId
           << "from" << filePath << "in" << timer.elapsed() << "ms";

  loadCollections();
}

void Storage::deleteWaypoint(qint64 collectionId, qint64 waypointId)
{
  if (!checkAccess("deleteWaypoint")){
    emit collectionDetailsLoaded(Collection(collectionId), false);
    return;
  }

  QSqlQuery sql(db);
  sql.prepare("DELETE FROM `waypoint` WHERE `id` = :id AND `collection_id` = :collection_id;");
  sql.bindValue(":id", waypointId);
  sql.bindValue(":collection_id", collectionId);
  sql.exec();

  if (sql.lastError().isValid()) {
    qWarning() << "Deleting waypoint failed" << sql.lastError();
    emit error(tr("Deleting waypoint failed: %1").arg(sql.lastError().text()));
    loadCollectionDetails(Collection(collectionId));
  }

  loadCollectionDetails(Collection(collectionId));
}

void Storage::createWaypoint(qint64 collectionId, double lat, double lon, QString name, QString description)
{
  if (!checkAccess("deleteTrack")){
    emit collectionDetailsLoaded(Collection(collectionId), false);
    return;
  }

  QSqlQuery sqlWpt(db);
  sqlWpt.prepare(
    "INSERT INTO `waypoint` (`collection_id`, `timestamp`, `latitude`, `longitude`, `name`, `description`) VALUES (:collection_id, :timestamp, :latitude, :longitude, :name, :description)");

  sqlWpt.bindValue(":collection_id", collectionId);
  sqlWpt.bindValue(":timestamp", QDateTime::currentDateTime());
  sqlWpt.bindValue(":latitude", lat);
  sqlWpt.bindValue(":longitude", lon);
  sqlWpt.bindValue(":name", name);
  sqlWpt.bindValue(":description", (description.isEmpty() ? QVariant() : description));

  sqlWpt.exec();
  if (sqlWpt.lastError().isValid()) {
    qWarning() << "Creation of waypoint failed" << sqlWpt.lastError();
    emit error(tr("Creation of waypoint failed: %1").arg(sqlWpt.lastError().text()));
  }

  loadCollectionDetails(Collection(collectionId));
}

void Storage::deleteTrack(qint64 collectionId, qint64 trackId)
{
  if (!checkAccess("deleteTrack")){
    emit collectionDetailsLoaded(Collection(collectionId), false);
    return;
  }

  QSqlQuery sql(db);
  sql.prepare("DELETE FROM `track` WHERE `id` = :id AND `collection_id` = :collection_id;");
  sql.bindValue(":id", trackId);
  sql.bindValue(":collection_id", collectionId);
  sql.exec();

  if (sql.lastError().isValid()) {
    qWarning() << "Deleting track failed" << sql.lastError();
    emit error(tr("Deleting track failed: %1").arg(sql.lastError().text()));
    loadCollectionDetails(Collection(collectionId));
  }

  loadCollectionDetails(Collection(collectionId));
}

void Storage::editWaypoint(qint64 collectionId, qint64 id, QString name, QString description)
{
  if (!checkAccess("editWaypoint")){
    emit collectionDetailsLoaded(Collection(collectionId), false);
    return;
  }

  QSqlQuery sql(db);
  sql.prepare("UPDATE `waypoint` SET `name` = :name, description = :description WHERE `id` = :id AND `collection_id` = :collection_id;");
  sql.bindValue(":id", id);
  sql.bindValue(":collection_id", collectionId);
  sql.bindValue(":name", name);
  sql.bindValue(":description", description);
  sql.exec();

  if (sql.lastError().isValid()) {
    qWarning() << "Edit waypoint failed" << sql.lastError();
    emit error(tr("Edit waypoint failed: %1").arg(sql.lastError().text()));
    loadCollectionDetails(Collection(collectionId));
  }

  loadCollectionDetails(Collection(collectionId));
}

void Storage::editTrack(qint64 collectionId, qint64 id, QString name, QString description)
{
  if (!checkAccess("editTrack")){
    emit collectionDetailsLoaded(Collection(collectionId), false);
    return;
  }

  QSqlQuery sql(db);
  sql.prepare("UPDATE `track` SET `name` = :name, description = :description WHERE `id` = :id AND `collection_id` = :collection_id;");
  sql.bindValue(":id", id);
  sql.bindValue(":collection_id", collectionId);
  sql.bindValue(":name", name);
  sql.bindValue(":description", description);
  sql.exec();

  if (sql.lastError().isValid()) {
    qWarning() << "Edit track failed" << sql.lastError();
    emit error(tr("Edit track failed: %1").arg(sql.lastError().text()));
    loadCollectionDetails(Collection(collectionId));
  }

  loadCollectionDetails(Collection(collectionId));
}

void Storage::exportCollection(qint64 collectionId, QString file)
{
  if (!checkAccess("exportCollection")){
    emit collectionExported(false);
    return;
  }

  QTime timer;
  timer.start();
  qDebug() << "Exporting collection" << collectionId << "to" << file;

  // load data
  Collection collection(collectionId);
  gpx::GpxFile gpxFile;
  if (!loadCollectionDetailsPrivate(collection)){
    emit collectionExported(false);
    return;
  }

  if (!collection.name.isEmpty()) {
    gpxFile.name = gpx::Optional<std::string>::of(collection.name.toStdString());
  }
  if (!collection.description.isEmpty()) {
    gpxFile.desc = gpx::Optional<std::string>::of(collection.description.toStdString());
  }

  assert(collection.waypoints);
  gpxFile.waypoints.reserve(collection.waypoints->size());
  for (const Waypoint &w: *(collection.waypoints)){
    gpxFile.waypoints.push_back(w.data);
  }
  collection.waypoints.reset();

  // load track data
  assert(collection.tracks);
  gpxFile.tracks.reserve(collection.tracks->size());
  for (Track &t : *(collection.tracks)){
    qDebug() << "Loading track data" << t.id;
    if (!loadTrackDataPrivate(t)){
      emit collectionExported(false);
      return;
    }
    assert(t.data);
    gpxFile.tracks.push_back(*(t.data));
    t.data.reset();
  }

  // export
  qDebug() << "Writing gpx file" << file;
  std::shared_ptr<ErrorCallback> callback = std::make_shared<ErrorCallback>();
  connect(callback.get(), SIGNAL(error(QString)), this, SIGNAL(error(QString)));

  bool success = gpx::ExportGpx(gpxFile,
                                file.toStdString(),
                                nullptr,
                                std::static_pointer_cast<gpx::ProcessCallback, ErrorCallback>(callback));

  qDebug() << "Exported in" << timer.elapsed() << "ms";

  emit collectionExported(success);
}

Storage::operator bool() const
{
  return ok;
}

Storage* Storage::getInstance()
{
  return storage;
}

void Storage::initInstance(const QDir &directory)
{
  if (storage == nullptr){
    QThread *thread = OSMScoutQt::GetInstance().makeThread("OverlayTileLoader");
    storage = new Storage(thread, directory);
    storage->moveToThread(thread);
    connect(thread, SIGNAL(started()),
            storage, SLOT(init()));
    thread->start();
  }
}

void Storage::clearInstance()
{
  if (storage != nullptr){
    storage->deleteLater();
    storage = nullptr;
  }
}
