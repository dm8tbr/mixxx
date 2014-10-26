#include <QFutureWatcher>
#include <QPixmapCache>
#include <QStringBuilder>
#include <QtConcurrentRun>

#include "library/coverartcache.h"
#include "library/coverartutils.h"
#include "soundsourceproxy.h"

// Large cover art wastes space in our cache when we typicaly won't show them at
// their full size. This is the max side length we resize images to.
const int kMaxCoverSize = 300;

CoverArtCache::CoverArtCache()
        : m_pCoverArtDAO(NULL),
          m_pTrackDAO(NULL) {
    // The initial QPixmapCache limit is 10MB.
    // But it is not used just by the coverArt stuff,
    // it is also used by Qt to handle other things behind the scenes.
    // Consequently coverArt cache will always have less than those
    // 10MB available to store the pixmaps.
    // So, we must increase this size a bit more,
    // in order to allow CoverCache handle more covers (performance gain).
    QPixmapCache::setCacheLimit(20480);
}

CoverArtCache::~CoverArtCache() {
    qDebug() << "~CoverArtCache()";

    // The queue of updates might have some covers/tracks
    // waiting for db insert.
    // So, we force the update in order to save everything
    // before the destruction...
    updateDB(true);
}

void CoverArtCache::setCoverArtDAO(CoverArtDAO* coverdao) {
    m_pCoverArtDAO = coverdao;
}

void CoverArtCache::setTrackDAO(TrackDAO* trackdao) {
    m_pTrackDAO = trackdao;
}

QString CoverArtCache::trackInDBHash(int trackId) {
    return m_queueOfUpdates.value(trackId).first;
}

bool CoverArtCache::changeCoverArt(int trackId,
                                   const QString& newCoverLocation) {
    if (trackId < 1 || m_pCoverArtDAO == NULL || m_pTrackDAO == NULL) {
        return false;
    }

    m_queueOfUpdates.remove(trackId);

    if (newCoverLocation.isEmpty()) {
        m_pTrackDAO->updateCoverArt(trackId, -1);
        return true;
    }

    QImage img;
    if (newCoverLocation == "ID3TAG") {
        QString trackLocation = m_pTrackDAO->getTrackLocation(trackId);
        img = CoverArtUtils::extractEmbeddedCover(trackLocation);
    } else {
        img = QImage(newCoverLocation);
    }

    if (img.isNull()) {
        return false;
    }
    img = CoverArtUtils::maybeResizeImage(img, kMaxCoverSize);
    QString hash = CoverArtUtils::calculateHash(img);

    // Update DB
    int coverId = m_pCoverArtDAO->saveCoverArt(newCoverLocation, hash);
    m_pTrackDAO->updateCoverArt(trackId, coverId);

    QPixmap pixmap;
    if (QPixmapCache::find(hash, &pixmap)) {
        emit(pixmapFound(trackId, pixmap));
        emit(requestRepaint());
    } else {
        pixmap.convertFromImage(img);
        if (QPixmapCache::insert(hash, pixmap)) {
            emit(pixmapFound(trackId, pixmap));
            emit(requestRepaint());
        }
    }

    return true;
}

QPixmap CoverArtCache::requestPixmap(const CoverInfo& requestInfo,
                                     const QSize& croppedSize,
                                     const bool onlyCached,
                                     const bool issueRepaint) {
    CoverInfo info = requestInfo;
    if (info.trackId < 1 || m_pCoverArtDAO == NULL) {
        return QPixmap();
    }

    // keep a list of trackIds for which a future is currently running
    // to avoid loading the same picture again while we are loading it
    if (m_runningIds.contains(info.trackId)) {
        return QPixmap();
    }

    // check if we have already found a cover for this track
    // and if it is just waiting to be inserted/updated in the DB.
    // In this case, we update the coverLocation and the hash.
    if (m_queueOfUpdates.contains(info.trackId)) {
        info.coverLocation = m_queueOfUpdates[info.trackId].first;
        info.hash = m_queueOfUpdates[info.trackId].second;
    }

    // If this request comes from CoverDelegate (table view),
    // it'll want to get a cropped cover which is ready to be drawn
    // in the table view (cover art column).
    // It's very important to keep the cropped covers in cache because it avoids
    // having to rescale+crop it ALWAYS (which brings a lot of performance issues).
    QString cacheKey = QString("CoverArtCache_%1_%2x%3")
                           .arg(info.hash)
                           .arg(croppedSize.width())
                           .arg(croppedSize.height());

    QPixmap pixmap;
    if (QPixmapCache::find(cacheKey, &pixmap)) {
        if (!issueRepaint) {
            emit(pixmapFound(info.trackId, pixmap));
        }
        return pixmap;
    }

    if (onlyCached) {
        return QPixmap();
    }

    QFuture<FutureResult> future;
    QFutureWatcher<FutureResult>* watcher = new QFutureWatcher<FutureResult>(this);
    CoverArtDAO::CoverArtInfo coverInfo;
    if (info.hash.isEmpty() ||
           (info.coverLocation != "ID3TAG" && !QFile::exists(info.coverLocation))) {
        coverInfo = m_pCoverArtDAO->getCoverArtInfo(info.trackId);
        future = QtConcurrent::run(this, &CoverArtCache::searchImage,
                                   coverInfo, croppedSize, issueRepaint);
        connect(watcher, SIGNAL(finished()), this, SLOT(imageFound()));
    } else {
        coverInfo.trackId = info.trackId;
        coverInfo.coverLocation = info.coverLocation;
        coverInfo.hash = info.hash;
        coverInfo.trackLocation = info.trackLocation;
        future = QtConcurrent::run(this, &CoverArtCache::loadImage,
                                   coverInfo, croppedSize, issueRepaint);
        connect(watcher, SIGNAL(finished()), this, SLOT(imageLoaded()));
    }
    m_runningIds.insert(info.trackId);
    watcher->setFuture(future);

    return QPixmap();
}

// Load cover from path stored in DB.
// It is executed in a separate thread via QtConcurrent::run
CoverArtCache::FutureResult CoverArtCache::loadImage(CoverArtDAO::CoverArtInfo coverInfo,
                                                     const QSize& croppedSize,
                                                     const bool issueRepaint) {
    FutureResult res;
    res.trackId = coverInfo.trackId;
    res.coverLocation = coverInfo.coverLocation;
    res.hash = coverInfo.hash;
    res.croppedSize = croppedSize;
    res.issueRepaint = issueRepaint;

    if (res.coverLocation == "ID3TAG") {
        res.img = CoverArtUtils::extractEmbeddedCover(coverInfo.trackLocation);
    } else {
        res.img = QImage(res.coverLocation);
    }

    if (res.croppedSize.isNull()) {
        res.img = CoverArtUtils::maybeResizeImage(res.img, kMaxCoverSize);
    } else {
        res.img = CoverArtUtils::cropImage(res.img, res.croppedSize);
    }

    return res;
}

// watcher
void CoverArtCache::imageLoaded() {
    QFutureWatcher<FutureResult>* watcher;
    watcher = reinterpret_cast<QFutureWatcher<FutureResult>*>(sender());
    FutureResult res = watcher->result();

    QString cacheKey = QString("CoverArtCache_%1_%2x%3")
                           .arg(res.hash)
                           .arg(res.croppedSize.width())
                           .arg(res.croppedSize.height());

    QPixmap pixmap;
    QPixmapCache::find(cacheKey, &pixmap);
    if (pixmap.isNull() && !res.img.isNull()) {
        pixmap.convertFromImage(res.img);
        QPixmapCache::insert(cacheKey, pixmap);
    }

    if (res.issueRepaint) {
        emit(requestRepaint());
    } else {
        emit(pixmapFound(res.trackId, pixmap));
    }

    m_runningIds.remove(res.trackId);
}

// Searching and loading QImages is a very slow process
// that could block the main thread. Therefore, this method
// is executed in a separate thread via QtConcurrent::run
CoverArtCache::FutureResult CoverArtCache::searchImage(
                                           CoverArtDAO::CoverArtInfo coverInfo,
                                           const QSize& croppedSize,
                                           const bool issueRepaint) {
    FutureResult res;
    res.trackId = coverInfo.trackId;
    res.croppedSize = croppedSize;
    res.issueRepaint = issueRepaint;

    // Looking for embedded cover art.
    res.img = CoverArtUtils::extractEmbeddedCover(coverInfo.trackLocation);
    if (!res.img.isNull()) {
        res.coverLocation = "ID3TAG";
        res.img = CoverArtUtils::maybeResizeImage(res.img, kMaxCoverSize);
    }

    // Looking for cover stored in track diretory.
    if (res.img.isNull()) {
        res.coverLocation = CoverArtUtils::searchInTrackDirectory(
            coverInfo.trackDirectory, coverInfo.trackBaseName, coverInfo.album);
        res.img = CoverArtUtils::maybeResizeImage(QImage(res.coverLocation),
                                                  kMaxCoverSize);
    }

    res.hash = CoverArtUtils::calculateHash(res.img);

    // adjusting the cover size according to the final purpose
    if (!res.croppedSize.isNull()) {
        res.img = CoverArtUtils::cropImage(res.img, res.croppedSize);
    }

    return res;
}

// watcher
void CoverArtCache::imageFound() {
    QFutureWatcher<FutureResult>* watcher;
    watcher = reinterpret_cast<QFutureWatcher<FutureResult>*>(sender());
    FutureResult res = watcher->result();

    QString cacheKey = QString("CoverArtCache_%1_%2x%3")
                           .arg(res.hash)
                           .arg(res.croppedSize.width())
                           .arg(res.croppedSize.height());

    QPixmap pixmap;
    QPixmapCache::find(cacheKey, &pixmap);
    if (pixmap.isNull()) {
        pixmap.convertFromImage(res.img);
        QPixmapCache::insert(cacheKey, pixmap);
    }

    if (res.issueRepaint) {
        emit(requestRepaint());
    } else {
        emit(pixmapFound(res.trackId, pixmap));
    }

    // update DB
    if (!m_queueOfUpdates.contains(res.trackId)) {
        m_queueOfUpdates.insert(res.trackId,
                                qMakePair(res.coverLocation, res.hash));
        updateDB();
    }

    m_runningIds.remove(res.trackId);
}

// sqlite can't do a huge number of updates in a very short time,
// so it is important to collect all new covers and write them at once.
void CoverArtCache::updateDB(bool forceUpdate) {
    if (!m_pCoverArtDAO || !m_pTrackDAO ||
            (!forceUpdate && m_queueOfUpdates.size() < 500)) {
        return;
    }

    qDebug() << "CoverCache : Updating" << m_queueOfUpdates.size() << "tracks!";

    QSet<QPair<int, int> > covers;
    covers = m_pCoverArtDAO->saveCoverArt(m_queueOfUpdates);
    m_pTrackDAO->updateCoverArt(covers);
    m_queueOfUpdates.clear();
}
