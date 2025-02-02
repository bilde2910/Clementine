/* This file is part of Clementine.
   Copyright 2013, David Sansome <me@davidsansome.com>

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

#include "tagreader.h"

#include <aifffile.h>
#include <apefile.h>
#include <asffile.h>
#include <attachedpictureframe.h>
#include <commentsframe.h>
#include <fileref.h>
#include <flacfile.h>
#include <id3v2tag.h>
#include <mp4file.h>
#include <mp4tag.h>
#include <mpcfile.h>
#include <mpegfile.h>
#include <oggfile.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QTextCodec>
#include <QUrl>
#include <QVector>
#include <memory>
#ifdef TAGLIB_HAS_OPUS
#include <opusfile.h>
#endif
#include <apetag.h>
#include <oggflacfile.h>
#include <popularimeterframe.h>
#include <speexfile.h>
#include <sys/stat.h>
#include <tag.h>
#include <tdebuglistener.h>
#include <textidentificationframe.h>
#include <trueaudiofile.h>
#include <tstring.h>
#include <unsynchronizedlyricsframe.h>
#include <vorbisfile.h>
#include <wavfile.h>
#include <wavpackfile.h>

#include "core/logging.h"
#include "core/messagehandler.h"
#include "core/timeconstants.h"
#include "fmpsparser.h"
#include "gmereader.h"

// Taglib added support for FLAC pictures in 1.7.0
#if (TAGLIB_MAJOR_VERSION > 1) || \
    (TAGLIB_MAJOR_VERSION == 1 && TAGLIB_MINOR_VERSION >= 7)
#define TAGLIB_HAS_FLAC_PICTURELIST
#endif

#ifdef HAVE_GOOGLE_DRIVE
#include "cloudstream.h"
#endif

#define NumberToASFAttribute(x) \
  TagLib::ASF::Attribute(QStringToTaglibString(QString::number(x)))

class TagLibFileRefFactory : public FileRefFactory {
 public:
  virtual TagLib::FileRef* GetFileRef(const QString& filename) {
#ifdef Q_OS_WIN32
    return new TagLib::FileRef(filename.toStdWString().c_str());
#else
    return new TagLib::FileRef(QFile::encodeName(filename).constData());
#endif
  }
};

// Handler to push TagLib messages to qLog instead of printing to stderr.
class TagReaderDebugListener : public TagLib::DebugListener {
 private:
  TagReaderDebugListener() {
    // Install handler.
    TagLib::setDebugListener(this);
  }

  virtual void printMessage(const TagLib::String& msg) override {
    // Remove trailing newline.
    qLog(Debug).noquote() << TStringToQString(msg).trimmed();
  }
  static TagReaderDebugListener listener_;
};
TagReaderDebugListener TagReaderDebugListener::listener_;

namespace {

TagLib::String StdStringToTaglibString(const std::string& s) {
  return TagLib::String(s.c_str(), TagLib::String::UTF8);
}

TagLib::String QStringToTaglibString(const QString& s) {
  return TagLib::String(s.toUtf8().constData(), TagLib::String::UTF8);
}
}  // namespace

const char* TagReader::kMP4_FMPS_Rating_ID =
    "----:com.apple.iTunes:FMPS_Rating";
const char* TagReader::kMP4_FMPS_Playcount_ID =
    "----:com.apple.iTunes:FMPS_Playcount";
const char* TagReader::kMP4_FMPS_Score_ID =
    "----:com.apple.iTunes:FMPS_Rating_Amarok_Score";

namespace {
// Tags containing the year the album was originally released (in contrast to
// other tags that contain the release year of the current edition)
const char* kMP4_OriginalYear_ID = "----:com.apple.iTunes:ORIGINAL YEAR";
const char* kASF_OriginalDate_ID = "WM/OriginalReleaseTime";
const char* kASF_OriginalYear_ID = "WM/OriginalReleaseYear";
}  // namespace

TagReader::TagReader()
    : factory_(new TagLibFileRefFactory), kEmbeddedCover("(embedded)") {}

void TagReader::ReadFile(const QString& filename,
                         cpb::tagreader::SongMetadata* song) const {
  const QByteArray url(QUrl::fromLocalFile(filename).toEncoded());
  const QFileInfo info(filename);

  song->set_basefilename(DataCommaSizeFromQString(info.fileName()));
  song->set_url(url.constData(), url.size());
  song->set_filesize(info.size());

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
  qint64 mtime = info.lastModified().toSecsSinceEpoch();
  qint64 btime = mtime;
  if (info.birthTime().isValid()) {
    btime = info.birthTime().toSecsSinceEpoch();
  }
#elif QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
  qint64 mtime = info.lastModified().toSecsSinceEpoch();
  qint64 btime = info.created().toSecsSinceEpoch();
#else
  // Legacy 32bit API.
  uint mtime = info.lastModified().toTime_t();
  uint btime = info.created().toTime_t();
#endif

  song->set_mtime(mtime);
  // NOTE: birthtime isn't supported by all filesystems or NFS implementations.
  // -1 is often returned if not supported. Note further that for the
  // toTime_t() call this returns an unsigned int, i.e. UINT_MAX.
  if (btime == -1) {
    btime = mtime;
  }
  song->set_ctime(btime);

  qLog(Debug) << "Reading tags from" << filename << ". Got tags:"
              << "size=" << info.size() << "; mtime=" << mtime
              << "; birthtime=" << btime;

  std::unique_ptr<TagLib::FileRef> fileref(factory_->GetFileRef(filename));
  if (fileref->isNull()) {
    qLog(Info) << "TagLib hasn't been able to read " << filename << " file";

    // Try fallback -- GME filetypes
    GME::ReadFile(info, song);
    return;
  }

  TagLib::Tag* tag = fileref->tag();
  if (tag) {
    Decode(tag->title(), nullptr, song->mutable_title());
    Decode(tag->artist(), nullptr, song->mutable_artist());  // TPE1
    Decode(tag->album(), nullptr, song->mutable_album());
    Decode(tag->genre(), nullptr, song->mutable_genre());
    song->set_year(tag->year());
    song->set_track(tag->track());
    song->set_valid(true);
  }

  QString disc;
  QString compilation;
  QString lyrics;

  auto parseApeTag = [&](TagLib::APE::Tag* tag) {
    const TagLib::APE::ItemListMap& items = tag->itemListMap();

    // Find album artists
    TagLib::APE::ItemListMap::ConstIterator it = items.find("ALBUM ARTIST");
    if (it != items.end()) {
      TagLib::StringList album_artists = it->second.values();
      if (!album_artists.isEmpty()) {
        Decode(album_artists.front(), nullptr, song->mutable_albumartist());
      }
    }

    // Find album cover art
    if (items.find("COVER ART (FRONT)") != items.end()) {
      song->set_art_automatic(kEmbeddedCover);
    }

    if (items.contains("COMPILATION")) {
      compilation = TStringToQString(
          TagLib::String::number(items["COMPILATION"].toString().toInt()));
    }

    if (items.contains("DISC")) {
      disc = TStringToQString(
          TagLib::String::number(items["DISC"].toString().toInt()));
    }

    if (items.contains("FMPS_RATING")) {
      float rating =
          TStringToQString(items["FMPS_RATING"].toString()).toFloat();
      if (song->rating() <= 0 && rating > 0) {
        song->set_rating(rating);
      }
    }
    if (items.contains("FMPS_PLAYCOUNT")) {
      int playcount =
          TStringToQString(items["FMPS_PLAYCOUNT"].toString()).toFloat();
      if (song->playcount() <= 0 && playcount > 0) {
        song->set_playcount(playcount);
      }
    }
    if (items.contains("FMPS_RATING_AMAROK_SCORE")) {
      int score = TStringToQString(items["FMPS_RATING_AMAROK_SCORE"].toString())
                      .toFloat() *
                  100;
      if (song->score() <= 0 && score > 0) {
        song->set_score(score);
      }
    }

    if (items.contains("BPM")) {
      Decode(items["BPM"].values().toString(", "), nullptr,
             song->mutable_performer());
    }

    if (items.contains("PERFORMER")) {
      Decode(items["PERFORMER"].values().toString(", "), nullptr,
             song->mutable_performer());
    }

    if (items.contains("COMPOSER")) {
      Decode(items["COMPOSER"].values().toString(", "), nullptr,
             song->mutable_composer());
    }

    if (items.contains("GROUPING")) {
      Decode(items["GROUPING"].values().toString(" "), nullptr,
             song->mutable_grouping());
    }

    if (items.contains("LYRICS")) {
      Decode(items["LYRICS"].toString(), nullptr, song->mutable_lyrics());
    }

    Decode(tag->comment(), nullptr, song->mutable_comment());
  };

  // Handle all the files which have VorbisComments (Ogg, OPUS, ...) in the same
  // way;
  // apart, so we keep specific behavior for some formats by adding another
  // "else if" block below.
  if (TagLib::Ogg::XiphComment* tag =
          dynamic_cast<TagLib::Ogg::XiphComment*>(fileref->file()->tag())) {
    ParseOggTag(tag->fieldListMap(), nullptr, &disc, &compilation, song);
#if TAGLIB_MAJOR_VERSION >= 1 && TAGLIB_MINOR_VERSION >= 11
    if (!tag->pictureList().isEmpty()) song->set_art_automatic(kEmbeddedCover);
#endif
  }

  if (TagLib::MPEG::File* file =
          dynamic_cast<TagLib::MPEG::File*>(fileref->file())) {
    if (file->ID3v2Tag()) {
      const TagLib::ID3v2::FrameListMap& map = file->ID3v2Tag()->frameListMap();

      if (!map["TPOS"].isEmpty())
        disc = TStringToQString(map["TPOS"].front()->toString()).trimmed();

      if (!map["TBPM"].isEmpty())
        song->set_bpm(TStringToQString(map["TBPM"].front()->toString())
                          .trimmed()
                          .toFloat());

      if (!map["TCOM"].isEmpty())
        Decode(map["TCOM"].front()->toString(), nullptr,
               song->mutable_composer());

      if (!map["TIT1"].isEmpty())  // content group
        Decode(map["TIT1"].front()->toString(), nullptr,
               song->mutable_grouping());

      if (!map["TOPE"].isEmpty())  // original artist/performer
        Decode(map["TOPE"].front()->toString(), nullptr,
               song->mutable_performer());

      // Skip TPE1 (which is the artist) here because we already fetched it

      if (!map["TPE2"].isEmpty())  // non-standard: Apple, Microsoft
        Decode(map["TPE2"].front()->toString(), nullptr,
               song->mutable_albumartist());

      if (!map["TCMP"].isEmpty())
        compilation =
            TStringToQString(map["TCMP"].front()->toString()).trimmed();

      if (!map["TDOR"].isEmpty()) {
        song->set_originalyear(
            map["TDOR"].front()->toString().substr(0, 4).toInt());
      } else if (!map["TORY"].isEmpty()) {
        song->set_originalyear(
            map["TORY"].front()->toString().substr(0, 4).toInt());
      }

      if (!map["USLT"].isEmpty()) {
        Decode(map["USLT"].front()->toString(), nullptr,
               song->mutable_lyrics());
      } else if (!map["SYLT"].isEmpty()) {
        Decode(map["SYLT"].front()->toString(), nullptr,
               song->mutable_lyrics());
      }

      if (!map["APIC"].isEmpty()) song->set_art_automatic(kEmbeddedCover);

      // Find a suitable comment tag.  For now we ignore iTunNORM comments.
      for (int i = 0; i < map["COMM"].size(); ++i) {
        const TagLib::ID3v2::CommentsFrame* frame =
            dynamic_cast<const TagLib::ID3v2::CommentsFrame*>(map["COMM"][i]);

        if (frame && TStringToQString(frame->description()) != "iTunNORM") {
          Decode(frame->text(), nullptr, song->mutable_comment());
          break;
        }
      }

      // Parse FMPS frames
      for (int i = 0; i < map["TXXX"].size(); ++i) {
        const TagLib::ID3v2::UserTextIdentificationFrame* frame =
            dynamic_cast<const TagLib::ID3v2::UserTextIdentificationFrame*>(
                map["TXXX"][i]);

        if (frame && frame->description().startsWith("FMPS_")) {
          ParseFMPSFrame(TStringToQString(frame->description()),
                         TStringToQString(frame->fieldList()[1]), song);
        }
      }

      // Check POPM tags
      // We do this after checking FMPS frames, so FMPS have precedence, as we
      // will consider POPM tags iff song has no rating/playcount already set.
      if (!map["POPM"].isEmpty()) {
        const TagLib::ID3v2::PopularimeterFrame* frame =
            dynamic_cast<const TagLib::ID3v2::PopularimeterFrame*>(
                map["POPM"].front());
        if (frame) {
          // Take a user rating only if there's no rating already set
          if (song->rating() <= 0 && frame->rating() > 0) {
            song->set_rating(ConvertPOPMRating(frame->rating()));
          }
          if (song->playcount() <= 0 && frame->counter() > 0) {
            song->set_playcount(frame->counter());
          }
        }
      }
    }
  } else if (TagLib::FLAC::File* file =
                 dynamic_cast<TagLib::FLAC::File*>(fileref->file())) {
    if (file->xiphComment()) {
      ParseOggTag(file->xiphComment()->fieldListMap(), nullptr, &disc,
                  &compilation, song);
#ifdef TAGLIB_HAS_FLAC_PICTURELIST
      if (!file->pictureList().isEmpty()) {
        song->set_art_automatic(kEmbeddedCover);
      }
#endif
    }
    Decode(tag->comment(), nullptr, song->mutable_comment());
  } else if (TagLib::MP4::File* file =
                 dynamic_cast<TagLib::MP4::File*>(fileref->file())) {
    if (file->tag()) {
      TagLib::MP4::Tag* mp4_tag = file->tag();
      TagLib::MP4::Item item;

      // Find album artists
      item = mp4_tag->item("aART");
      if (item.isValid()) {
        TagLib::StringList album_artists = item.toStringList();
        if (!album_artists.isEmpty()) {
          Decode(album_artists.front(), nullptr, song->mutable_albumartist());
        }
      }

      // Find album cover art
      item = mp4_tag->item("covr");
      if (item.isValid()) {
        song->set_art_automatic(kEmbeddedCover);
      }

      item = mp4_tag->item("disk");
      if (item.isValid()) {
        disc = TStringToQString(TagLib::String::number(item.toIntPair().first));
      }

      item = mp4_tag->item(kMP4_FMPS_Rating_ID);
      if (item.isValid()) {
        float rating =
            TStringToQString(item.toStringList().toString('\n')).toFloat();
        if (song->rating() <= 0 && rating > 0) {
          song->set_rating(rating);
        }
      }
      item = mp4_tag->item(kMP4_FMPS_Playcount_ID);
      if (item.isValid()) {
        int playcount =
            TStringToQString(item.toStringList().toString('\n')).toFloat();
        if (song->playcount() <= 0 && playcount > 0) {
          song->set_playcount(playcount);
        }
      }
      item = mp4_tag->item(kMP4_FMPS_Score_ID);
      if (item.isValid()) {
        int score =
            TStringToQString(item.toStringList().toString('\n')).toFloat() *
            100;
        if (song->score() <= 0 && score > 0) {
          song->set_score(score);
        }
      }

      item = mp4_tag->item("\251wrt");
      if (item.isValid()) {
        Decode(item.toStringList().toString(", "), nullptr,
               song->mutable_composer());
      }
      item = mp4_tag->item("\251grp");
      if (item.isValid()) {
        Decode(item.toStringList().toString(" "), nullptr,
               song->mutable_grouping());
      }
      item = mp4_tag->item("\251lyr");
      if (item.isValid()) {
        Decode(item.toStringList().toString(" "), nullptr,
               song->mutable_lyrics());
      }

      item = mp4_tag->item(kMP4_OriginalYear_ID);
      if (item.isValid()) {
        song->set_originalyear(
            TStringToQString(item.toStringList().toString('\n'))
                .left(4)
                .toInt());
      }

      Decode(mp4_tag->comment(), nullptr, song->mutable_comment());
    }
  } else if (TagLib::APE::File* file =
                 dynamic_cast<TagLib::APE::File*>(fileref->file())) {
    if (file->tag()) {
      parseApeTag(file->APETag());
    }
  } else if (TagLib::MPC::File* file =
                 dynamic_cast<TagLib::MPC::File*>(fileref->file())) {
    if (file->tag()) {
      parseApeTag(file->APETag());
    }
  } else if (TagLib::WavPack::File* file =
                 dynamic_cast<TagLib::WavPack::File*>(fileref->file())) {
    if (file->tag()) {
      parseApeTag(file->APETag());
    }
  }
#ifdef TAGLIB_WITH_ASF
  else if (TagLib::ASF::File* file =
               dynamic_cast<TagLib::ASF::File*>(fileref->file())) {
    const TagLib::ASF::AttributeListMap& attributes_map =
        file->tag()->attributeListMap();
    if (attributes_map.contains("FMPS/Rating")) {
      const TagLib::ASF::AttributeList& attributes =
          attributes_map["FMPS/Rating"];
      if (!attributes.isEmpty()) {
        float rating =
            TStringToQString(attributes.front().toString()).toFloat();
        if (song->rating() <= 0 && rating > 0) {
          song->set_rating(rating);
        }
      }
    }
    if (attributes_map.contains("FMPS/Playcount")) {
      const TagLib::ASF::AttributeList& attributes =
          attributes_map["FMPS/Playcount"];
      if (!attributes.isEmpty()) {
        int playcount = TStringToQString(attributes.front().toString()).toInt();
        if (song->playcount() <= 0 && playcount > 0) {
          song->set_playcount(playcount);
        }
      }
    }
    if (attributes_map.contains("FMPS/Rating_Amarok_Score")) {
      const TagLib::ASF::AttributeList& attributes =
          attributes_map["FMPS/Rating_Amarok_Score"];
      if (!attributes.isEmpty()) {
        int score =
            TStringToQString(attributes.front().toString()).toFloat() * 100;
        if (song->score() <= 0 && score > 0) {
          song->set_score(score);
        }
      }
    }

    if (attributes_map.contains(kASF_OriginalDate_ID)) {
      const TagLib::ASF::AttributeList& attributes =
          attributes_map[kASF_OriginalDate_ID];
      if (!attributes.isEmpty()) {
        song->set_originalyear(
            TStringToQString(attributes.front().toString()).left(4).toInt());
      }
    } else if (attributes_map.contains(kASF_OriginalYear_ID)) {
      const TagLib::ASF::AttributeList& attributes =
          attributes_map[kASF_OriginalYear_ID];
      if (!attributes.isEmpty()) {
        song->set_originalyear(
            TStringToQString(attributes.front().toString()).left(4).toInt());
      }
    }
  }
#endif
  else if (tag) {
    Decode(tag->comment(), nullptr, song->mutable_comment());
  }

  if (!disc.isEmpty()) {
    const int i = disc.indexOf('/');
    if (i != -1) {
      // disc.right( i ).toInt() is total number of discs, we don't use this at
      // the moment
      song->set_disc(disc.left(i).toInt());
    } else {
      song->set_disc(disc.toInt());
    }
  }

  if (compilation.isEmpty()) {
    // well, it wasn't set, but if the artist is VA assume it's a compilation
    if (QStringFromStdString(song->artist()).toLower() == "various artists") {
      song->set_compilation(true);
    }
  } else {
    song->set_compilation(compilation.toInt() == 1);
  }

  if (!lyrics.isEmpty()) song->set_lyrics(lyrics.toStdString());

  if (fileref->audioProperties()) {
    song->set_bitrate(fileref->audioProperties()->bitrate());
    song->set_samplerate(fileref->audioProperties()->sampleRate());
    song->set_length_nanosec(fileref->audioProperties()->lengthInMilliseconds() *
                             kNsecPerMsec);
  }

  // Get the filetype if we can
  song->set_type(GuessFileType(fileref.get()));

// Set integer fields to -1 if they're not valid
#define SetDefault(field)   \
  if (song->field() <= 0) { \
    song->set_##field(-1);  \
  }
  SetDefault(track);
  SetDefault(disc);
  SetDefault(bpm);
  SetDefault(year);
  SetDefault(bitrate);
  SetDefault(samplerate);
  SetDefault(lastplayed);
#undef SetDefault
}

void TagReader::Decode(const TagLib::String& tag, const QTextCodec* codec,
                       std::string* output) {
  QString tmp;

  if (codec && tag.isLatin1()) {  // Never override UTF-8.
    const std::string fixed =
        QString::fromUtf8(tag.toCString(true)).toStdString();
    tmp = codec->toUnicode(fixed.c_str()).trimmed();
  } else {
    tmp = TStringToQString(tag).trimmed();
  }

  output->assign(DataCommaSizeFromQString(tmp));
}

void TagReader::Decode(const QString& tag, const QTextCodec* codec,
                       std::string* output) {
  if (!codec) {
    output->assign(DataCommaSizeFromQString(tag));
  } else {
    const QString decoded(codec->toUnicode(tag.toUtf8()));
    output->assign(DataCommaSizeFromQString(decoded));
  }
}

void TagReader::ParseFMPSFrame(const QString& name, const QString& value,
                               cpb::tagreader::SongMetadata* song) const {
  qLog(Debug) << "Parsing FMPSFrame" << name << ", " << value;
  FMPSParser parser;
  if (!parser.Parse(value) || parser.is_empty()) return;

  QVariant var;
  if (name == "FMPS_Rating") {
    var = parser.result()[0][0];
    if (var.type() == QVariant::Double) {
      song->set_rating(var.toDouble());
    }
  } else if (name == "FMPS_Rating_User") {
    // Take a user rating only if there's no rating already set
    if (song->rating() == -1 && parser.result()[0].count() >= 2) {
      var = parser.result()[0][1];
      if (var.type() == QVariant::Double) {
        song->set_rating(var.toDouble());
      }
    }
  } else if (name == "FMPS_PlayCount") {
    var = parser.result()[0][0];
    if (var.type() == QVariant::Double) {
      song->set_playcount(var.toDouble());
    }
  } else if (name == "FMPS_PlayCount_User") {
    // Take a user playcount only if there's no playcount already set
    if (song->playcount() == 0 && parser.result()[0].count() >= 2) {
      var = parser.result()[0][1];
      if (var.type() == QVariant::Double) {
        song->set_playcount(var.toDouble());
      }
    }
  } else if (name == "FMPS_Rating_Amarok_Score") {
    var = parser.result()[0][0];
    if (var.type() == QVariant::Double) {
      song->set_score(var.toFloat() * 100);
    }
  }
}

void TagReader::ParseOggTag(const TagLib::Ogg::FieldListMap& map,
                            const QTextCodec* codec, QString* disc,
                            QString* compilation,
                            cpb::tagreader::SongMetadata* song) const {
  if (!map["COMPOSER"].isEmpty())
    Decode(map["COMPOSER"].front(), codec, song->mutable_composer());
  if (!map["PERFORMER"].isEmpty())
    Decode(map["PERFORMER"].front(), codec, song->mutable_performer());
  if (!map["CONTENT GROUP"].isEmpty())
    Decode(map["CONTENT GROUP"].front(), codec, song->mutable_grouping());

  if (!map["ALBUMARTIST"].isEmpty()) {
    Decode(map["ALBUMARTIST"].front(), codec, song->mutable_albumartist());
  } else if (!map["ALBUM ARTIST"].isEmpty()) {
    Decode(map["ALBUM ARTIST"].front(), codec, song->mutable_albumartist());
  }

  if (!map["ORIGINALDATE"].isEmpty())
    song->set_originalyear(
        TStringToQString(map["ORIGINALDATE"].front()).left(4).toInt());
  else if (!map["ORIGINALYEAR"].isEmpty())
    song->set_originalyear(
        TStringToQString(map["ORIGINALYEAR"].front()).toInt());

  if (!map["BPM"].isEmpty())
    song->set_bpm(TStringToQString(map["BPM"].front()).trimmed().toFloat());

  if (!map["DISCNUMBER"].isEmpty())
    *disc = TStringToQString(map["DISCNUMBER"].front()).trimmed();

  if (!map["COMPILATION"].isEmpty())
    *compilation = TStringToQString(map["COMPILATION"].front()).trimmed();

  if (!map["COVERART"].isEmpty()) song->set_art_automatic(kEmbeddedCover);

  if (!map["METADATA_BLOCK_PICTURE"].isEmpty())
    song->set_art_automatic(kEmbeddedCover);

  if (!map["FMPS_RATING"].isEmpty() && song->rating() <= 0)
    song->set_rating(
        TStringToQString(map["FMPS_RATING"].front()).trimmed().toFloat());

  if (!map["FMPS_PLAYCOUNT"].isEmpty() && song->playcount() <= 0)
    song->set_playcount(
        TStringToQString(map["FMPS_PLAYCOUNT"].front()).trimmed().toFloat());

  if (!map["FMPS_RATING_AMAROK_SCORE"].isEmpty() && song->score() <= 0)
    song->set_score(TStringToQString(map["FMPS_RATING_AMAROK_SCORE"].front())
                        .trimmed()
                        .toFloat() *
                    100);

  if (!map["LYRICS"].isEmpty())
    Decode(map["LYRICS"].front(), codec, song->mutable_lyrics());
  else if (!map["UNSYNCEDLYRICS"].isEmpty())
    Decode(map["UNSYNCEDLYRICS"].front(), codec, song->mutable_lyrics());
}

void TagReader::SetVorbisComments(
    TagLib::Ogg::XiphComment* vorbis_comments,
    const cpb::tagreader::SongMetadata& song) const {
  vorbis_comments->addField("COMPOSER",
                            StdStringToTaglibString(song.composer()), true);
  vorbis_comments->addField("PERFORMER",
                            StdStringToTaglibString(song.performer()), true);
  vorbis_comments->addField("CONTENT GROUP",
                            StdStringToTaglibString(song.grouping()), true);
  vorbis_comments->addField(
      "BPM",
      QStringToTaglibString(song.bpm() <= 0 - 1 ? QString()
                                                : QString::number(song.bpm())),
      true);
  vorbis_comments->addField(
      "DISCNUMBER",
      QStringToTaglibString(song.disc() <= 0 ? QString()
                                             : QString::number(song.disc())),
      true);
  vorbis_comments->addField(
      "COMPILATION",
      QStringToTaglibString(song.compilation() ? QString::number(1)
                                               : QString()),
      true);

  // Try to be coherent, the two forms are used but the first one is preferred

  vorbis_comments->addField("ALBUMARTIST",
                            StdStringToTaglibString(song.albumartist()), true);
  vorbis_comments->removeFields("ALBUM ARTIST");

  vorbis_comments->addField("LYRICS", StdStringToTaglibString(song.lyrics()),
                            true);
  vorbis_comments->removeFields("UNSYNCEDLYRICS");
}

void TagReader::SetFMPSStatisticsVorbisComments(
    TagLib::Ogg::XiphComment* vorbis_comments,
    const cpb::tagreader::SongMetadata& song) const {
  if (song.playcount())
    vorbis_comments->addField("FMPS_PLAYCOUNT",
                              TagLib::String::number(song.playcount()), true);
  if (song.score())
    vorbis_comments->addField(
        "FMPS_RATING_AMAROK_SCORE",
        QStringToTaglibString(QString::number(song.score() / 100.0)), true);
}

void TagReader::SetFMPSRatingVorbisComments(
    TagLib::Ogg::XiphComment* vorbis_comments,
    const cpb::tagreader::SongMetadata& song) const {
  vorbis_comments->addField(
      "FMPS_RATING", QStringToTaglibString(QString::number(song.rating())),
      true);
}

cpb::tagreader::SongMetadata_Type TagReader::GuessFileType(
    TagLib::FileRef* fileref) const {
#ifdef TAGLIB_WITH_ASF
  if (dynamic_cast<TagLib::ASF::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_ASF;
#endif
  if (dynamic_cast<TagLib::FLAC::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_FLAC;
#ifdef TAGLIB_WITH_MP4
  if (dynamic_cast<TagLib::MP4::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_MP4;
#endif
  if (dynamic_cast<TagLib::MPC::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_MPC;
  if (dynamic_cast<TagLib::MPEG::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_MPEG;
  if (dynamic_cast<TagLib::Ogg::FLAC::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_OGGFLAC;
  if (dynamic_cast<TagLib::Ogg::Speex::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_OGGSPEEX;
  if (dynamic_cast<TagLib::Ogg::Vorbis::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_OGGVORBIS;
#ifdef TAGLIB_HAS_OPUS
  if (dynamic_cast<TagLib::Ogg::Opus::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_OGGOPUS;
#endif
  if (dynamic_cast<TagLib::RIFF::AIFF::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_AIFF;
  if (dynamic_cast<TagLib::RIFF::WAV::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_WAV;
  if (dynamic_cast<TagLib::TrueAudio::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_TRUEAUDIO;
  if (dynamic_cast<TagLib::WavPack::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_WAVPACK;
  if (dynamic_cast<TagLib::APE::File*>(fileref->file()))
    return cpb::tagreader::SongMetadata_Type_APE;

  return cpb::tagreader::SongMetadata_Type_UNKNOWN;
}

bool TagReader::SaveFile(const QString& filename,
                         const cpb::tagreader::SongMetadata& song) const {
  if (filename.isNull()) return false;

  qLog(Debug) << "Saving tags to" << filename;

  std::unique_ptr<TagLib::FileRef> fileref(factory_->GetFileRef(filename));

  if (!fileref || fileref->isNull())  // The file probably doesn't exist
    return false;

  fileref->tag()->setTitle(StdStringToTaglibString(song.title()));
  fileref->tag()->setArtist(StdStringToTaglibString(song.artist()));  // TPE1
  fileref->tag()->setAlbum(StdStringToTaglibString(song.album()));
  fileref->tag()->setGenre(StdStringToTaglibString(song.genre()));
  fileref->tag()->setComment(StdStringToTaglibString(song.comment()));
  fileref->tag()->setYear(song.year() <= 0 - 1 ? 0 : song.year());
  fileref->tag()->setTrack(song.track() <= 0 - 1 ? 0 : song.track());

  auto saveApeTag = [&](TagLib::APE::Tag* tag) {
    tag->addValue(
        "disc",
        QStringToTaglibString(song.disc() <= 0 ? QString()
                                               : QString::number(song.disc())),
        true);
    tag->addValue("bpm",
                  QStringToTaglibString(song.bpm() <= 0 - 1
                                            ? QString()
                                            : QString::number(song.bpm())),
                  true);
    tag->setItem("composer",
                 TagLib::APE::Item(
                     "composer", TagLib::StringList(song.composer().c_str())));
    tag->setItem("grouping",
                 TagLib::APE::Item(
                     "grouping", TagLib::StringList(song.grouping().c_str())));
    tag->setItem("performer",
                 TagLib::APE::Item("performer", TagLib::StringList(
                                                    song.performer().c_str())));
    tag->setItem(
        "album artist",
        TagLib::APE::Item("album artist",
                          TagLib::StringList(song.albumartist().c_str())));
    tag->setItem("lyrics",
                 TagLib::APE::Item("lyrics", TagLib::String(song.lyrics())));
    tag->addValue("compilation",
                  QStringToTaglibString(song.compilation() ? QString::number(1)
                                                           : QString()),
                  true);
  };

  if (TagLib::MPEG::File* file =
          dynamic_cast<TagLib::MPEG::File*>(fileref->file())) {
    TagLib::ID3v2::Tag* tag = file->ID3v2Tag(true);
    SetTextFrame("TPOS",
                 song.disc() <= 0 ? QString() : QString::number(song.disc()),
                 tag);
    SetTextFrame("TBPM",
                 song.bpm() <= 0 - 1 ? QString() : QString::number(song.bpm()),
                 tag);
    SetTextFrame("TCOM", song.composer(), tag);
    SetTextFrame("TIT1", song.grouping(), tag);
    SetTextFrame("TOPE", song.performer(), tag);
    SetUnsyncLyricsFrame(song.lyrics(), tag);
    // Skip TPE1 (which is the artist) here because we already set it
    SetTextFrame("TPE2", song.albumartist(), tag);
    SetTextFrame("TCMP", song.compilation() ? QString::number(1) : QString(),
                 tag);
  } else if (TagLib::FLAC::File* file =
                 dynamic_cast<TagLib::FLAC::File*>(fileref->file())) {
    TagLib::Ogg::XiphComment* tag = file->xiphComment();
    SetVorbisComments(tag, song);
  } else if (TagLib::MP4::File* file =
                 dynamic_cast<TagLib::MP4::File*>(fileref->file())) {
    TagLib::MP4::Tag* tag = file->tag();
    tag->setItem("disk",
                 TagLib::MP4::Item(song.disc() <= 0 - 1 ? 0 : song.disc(), 0));
    tag->setItem("tmpo",
                 TagLib::StringList(song.bpm() <= 0 - 1
                                        ? "0"
                                        : TagLib::String::number(song.bpm())));
    tag->setItem("\251wrt", TagLib::StringList(song.composer().c_str()));
    tag->setItem("\251grp", TagLib::StringList(song.grouping().c_str()));
    tag->setItem("\251lyr", TagLib::StringList(song.lyrics().c_str()));
    tag->setItem("aART", TagLib::StringList(song.albumartist().c_str()));
    tag->setItem("cpil", TagLib::StringList(song.compilation() ? "1" : "0"));
  } else if (TagLib::APE::File* file =
                 dynamic_cast<TagLib::APE::File*>(fileref->file())) {
    saveApeTag(file->APETag(true));
  } else if (TagLib::MPC::File* file =
                 dynamic_cast<TagLib::MPC::File*>(fileref->file())) {
    saveApeTag(file->APETag(true));
  } else if (TagLib::WavPack::File* file =
                 dynamic_cast<TagLib::WavPack::File*>(fileref->file())) {
    saveApeTag(file->APETag(true));
  }

  // Handle all the files which have VorbisComments (Ogg, OPUS, ...) in the same
  // way;
  // apart, so we keep specific behavior for some formats by adding another
  // "else if" block above.
  if (TagLib::Ogg::XiphComment* tag =
          dynamic_cast<TagLib::Ogg::XiphComment*>(fileref->file()->tag())) {
    SetVorbisComments(tag, song);
  }

  bool ret = fileref->save();
#ifdef Q_OS_LINUX
  if (ret) {
    // Linux: inotify doesn't seem to notice the change to the file unless we
    // change the timestamps as well. (this is what touch does)
    utimensat(0, QFile::encodeName(filename).constData(), nullptr, 0);
  }
#endif  // Q_OS_LINUX

  return ret;
}

bool TagReader::SaveSongStatisticsToFile(
    const QString& filename, const cpb::tagreader::SongMetadata& song) const {
  if (filename.isNull()) return false;

  qLog(Debug) << "Saving song statistics tags to" << filename;

  std::unique_ptr<TagLib::FileRef> fileref(factory_->GetFileRef(filename));

  if (!fileref || fileref->isNull())  // The file probably doesn't exist
    return false;

  auto saveApeSongStats = [&](TagLib::APE::Tag* tag) {
    if (song.score())
      tag->setItem(
          "FMPS_Rating_Amarok_Score",
          TagLib::APE::Item(
              "FMPS_Rating_Amarok_Score",
              QStringToTaglibString(QString::number(song.score() / 100.0))));
    if (song.playcount())
      tag->setItem("FMPS_PlayCount",
                   TagLib::APE::Item("FMPS_PlayCount",
                                     TagLib::String::number(song.playcount())));
  };

  if (TagLib::MPEG::File* file =
          dynamic_cast<TagLib::MPEG::File*>(fileref->file())) {
    TagLib::ID3v2::Tag* tag = file->ID3v2Tag(true);

    if (song.playcount()) {
      // Save as FMPS
      SetUserTextFrame("FMPS_PlayCount", QString::number(song.playcount()),
                       tag);

      // Also save as POPM
      TagLib::ID3v2::PopularimeterFrame* frame = GetPOPMFrameFromTag(tag);
      frame->setCounter(song.playcount());
    }

    if (song.score())
      SetUserTextFrame("FMPS_Rating_Amarok_Score",
                       QString::number(song.score() / 100.0), tag);

  } else if (TagLib::FLAC::File* file =
                 dynamic_cast<TagLib::FLAC::File*>(fileref->file())) {
    TagLib::Ogg::XiphComment* vorbis_comments = file->xiphComment(true);
    SetFMPSStatisticsVorbisComments(vorbis_comments, song);
  } else if (TagLib::Ogg::XiphComment* tag =
                 dynamic_cast<TagLib::Ogg::XiphComment*>(
                     fileref->file()->tag())) {
    SetFMPSStatisticsVorbisComments(tag, song);
  }
#ifdef TAGLIB_WITH_ASF
  else if (TagLib::ASF::File* file =
               dynamic_cast<TagLib::ASF::File*>(fileref->file())) {
    TagLib::ASF::Tag* tag = file->tag();
    if (song.playcount())
      tag->addAttribute("FMPS/Playcount",
                        NumberToASFAttribute(song.playcount()));
    if (song.score())
      tag->addAttribute("FMPS/Rating_Amarok_Score",
                        NumberToASFAttribute(song.score() / 100.0));
  }
#endif
  else if (TagLib::MP4::File* file =
               dynamic_cast<TagLib::MP4::File*>(fileref->file())) {
    TagLib::MP4::Tag* tag = file->tag();
    if (song.score())
      tag->setItem(kMP4_FMPS_Score_ID,
                   TagLib::MP4::Item(QStringToTaglibString(
                       QString::number(song.score() / 100.0))));
    if (song.playcount())
      tag->setItem(kMP4_FMPS_Playcount_ID,
                   TagLib::MP4::Item(TagLib::String::number(song.playcount())));
  } else if (TagLib::APE::File* file =
                 dynamic_cast<TagLib::APE::File*>(fileref->file())) {
    saveApeSongStats(file->APETag(true));
  } else if (TagLib::MPC::File* file =
                 dynamic_cast<TagLib::MPC::File*>(fileref->file())) {
    saveApeSongStats(file->APETag(true));
  } else if (TagLib::WavPack::File* file =
                 dynamic_cast<TagLib::WavPack::File*>(fileref->file())) {
    saveApeSongStats(file->APETag(true));
  } else {
    // Nothing to save: stop now
    return true;
  }

  bool ret = fileref->save();
#ifdef Q_OS_LINUX
  if (ret) {
    // Linux: inotify doesn't seem to notice the change to the file unless we
    // change the timestamps as well. (this is what touch does)
    utimensat(0, QFile::encodeName(filename).constData(), nullptr, 0);
  }
#endif  // Q_OS_LINUX
  return ret;
}

bool TagReader::SaveSongRatingToFile(
    const QString& filename, const cpb::tagreader::SongMetadata& song) const {
  if (filename.isNull()) return false;

  qLog(Debug) << "Saving song rating tags to" << filename;
  if (song.rating() < 0) {
    // The FMPS spec says unrated == "tag not present". For us, no rating
    // results in rating being -1, so don't write anything in that case.
    // Actually, we should also remove tag set in this case, but in
    // Clementine it is not possible to unset rating i.e. make a song "unrated".
    qLog(Debug) << "Unrated: do nothing";
    return true;
  }

  std::unique_ptr<TagLib::FileRef> fileref(factory_->GetFileRef(filename));

  if (!fileref || fileref->isNull())  // The file probably doesn't exist
    return false;

  auto saveApeSongRating = [&](TagLib::APE::Tag* tag) {
    tag->setItem("FMPS_Rating",
                 TagLib::APE::Item("FMPS_Rating",
                                   TagLib::StringList(QStringToTaglibString(
                                       QString::number(song.rating())))));
  };

  if (TagLib::MPEG::File* file =
          dynamic_cast<TagLib::MPEG::File*>(fileref->file())) {
    TagLib::ID3v2::Tag* tag = file->ID3v2Tag(true);

    // Save as FMPS
    SetUserTextFrame("FMPS_Rating", QString::number(song.rating()), tag);

    // Also save as POPM
    TagLib::ID3v2::PopularimeterFrame* frame = GetPOPMFrameFromTag(tag);
    frame->setRating(ConvertToPOPMRating(song.rating()));

  } else if (TagLib::FLAC::File* file =
                 dynamic_cast<TagLib::FLAC::File*>(fileref->file())) {
    TagLib::Ogg::XiphComment* vorbis_comments = file->xiphComment(true);
    SetFMPSRatingVorbisComments(vorbis_comments, song);
  } else if (TagLib::Ogg::XiphComment* tag =
                 dynamic_cast<TagLib::Ogg::XiphComment*>(
                     fileref->file()->tag())) {
    SetFMPSRatingVorbisComments(tag, song);
  }
#ifdef TAGLIB_WITH_ASF
  else if (TagLib::ASF::File* file =
               dynamic_cast<TagLib::ASF::File*>(fileref->file())) {
    TagLib::ASF::Tag* tag = file->tag();
    tag->addAttribute("FMPS/Rating", NumberToASFAttribute(song.rating()));
  }
#endif
  else if (TagLib::MP4::File* file =
               dynamic_cast<TagLib::MP4::File*>(fileref->file())) {
    TagLib::MP4::Tag* tag = file->tag();
    tag->setItem(kMP4_FMPS_Rating_ID, TagLib::StringList(QStringToTaglibString(
                                          QString::number(song.rating()))));
  } else if (TagLib::APE::File* file =
                 dynamic_cast<TagLib::APE::File*>(fileref->file())) {
    saveApeSongRating(file->APETag(true));
  } else if (TagLib::MPC::File* file =
                 dynamic_cast<TagLib::MPC::File*>(fileref->file())) {
    saveApeSongRating(file->APETag(true));
  } else if (TagLib::WavPack::File* file =
                 dynamic_cast<TagLib::WavPack::File*>(fileref->file())) {
    saveApeSongRating(file->APETag(true));
  } else {
    // Nothing to save: stop now
    return true;
  }

  bool ret = fileref->save();
#ifdef Q_OS_LINUX
  if (ret) {
    // Linux: inotify doesn't seem to notice the change to the file unless we
    // change the timestamps as well. (this is what touch does)
    utimensat(0, QFile::encodeName(filename).constData(), nullptr, 0);
  }
#endif  // Q_OS_LINUX
  return ret;
}

void TagReader::SetUserTextFrame(const QString& description,
                                 const QString& value,
                                 TagLib::ID3v2::Tag* tag) const {
  const QByteArray descr_utf8(description.toUtf8());
  const QByteArray value_utf8(value.toUtf8());
  qLog(Debug) << "Setting FMPSFrame:" << description << ", " << value;
  SetUserTextFrame(std::string(descr_utf8.constData(), descr_utf8.length()),
                   std::string(value_utf8.constData(), value_utf8.length()),
                   tag);
}

void TagReader::SetUserTextFrame(const std::string& description,
                                 const std::string& value,
                                 TagLib::ID3v2::Tag* tag) const {
  const TagLib::String t_description = StdStringToTaglibString(description);
  // Remove the frame if it already exists
  TagLib::ID3v2::UserTextIdentificationFrame* frame =
      TagLib::ID3v2::UserTextIdentificationFrame::find(tag, t_description);
  if (frame) {
    tag->removeFrame(frame);
  }

  // Create and add a new frame
  frame = new TagLib::ID3v2::UserTextIdentificationFrame(TagLib::String::UTF8);

  frame->setDescription(t_description);
  frame->setText(StdStringToTaglibString(value));
  tag->addFrame(frame);
}

void TagReader::SetTextFrame(const char* id, const QString& value,
                             TagLib::ID3v2::Tag* tag) const {
  const QByteArray utf8(value.toUtf8());
  SetTextFrame(id, std::string(utf8.constData(), utf8.length()), tag);
}

void TagReader::SetTextFrame(const char* id, const std::string& value,
                             TagLib::ID3v2::Tag* tag) const {
  TagLib::ByteVector id_vector(id);
  QVector<TagLib::ByteVector> frames_buffer;

  // Store and clear existing frames
  while (tag->frameListMap().contains(id_vector) &&
         tag->frameListMap()[id_vector].size() != 0) {
    frames_buffer.push_back(tag->frameListMap()[id_vector].front()->render());
    tag->removeFrame(tag->frameListMap()[id_vector].front());
  }

  // If no frames stored create empty frame
  if (frames_buffer.isEmpty()) {
    TagLib::ID3v2::TextIdentificationFrame frame(id_vector,
                                                 TagLib::String::UTF8);
    frames_buffer.push_back(frame.render());
  }

  // Update and add the frames
  for (int lyrics_index = 0; lyrics_index < frames_buffer.size();
       lyrics_index++) {
    TagLib::ID3v2::TextIdentificationFrame* frame =
        new TagLib::ID3v2::TextIdentificationFrame(
            frames_buffer.at(lyrics_index));
    if (lyrics_index == 0) {
      frame->setText(StdStringToTaglibString(value));
    }
    // add frame takes ownership and clears the memory
    tag->addFrame(frame);
  }
}

void TagReader::SetUnsyncLyricsFrame(const std::string& value,
                                     TagLib::ID3v2::Tag* tag) const {
  TagLib::ByteVector id_vector("USLT");
  QVector<TagLib::ByteVector> frames_buffer;

  // Store and clear existing frames
  while (tag->frameListMap().contains(id_vector) &&
         tag->frameListMap()[id_vector].size() != 0) {
    frames_buffer.push_back(tag->frameListMap()[id_vector].front()->render());
    tag->removeFrame(tag->frameListMap()[id_vector].front());
  }

  // If no frames stored create empty frame
  if (frames_buffer.isEmpty()) {
    TagLib::ID3v2::UnsynchronizedLyricsFrame frame(TagLib::String::UTF8);
    frame.setDescription("Clementine editor");
    frames_buffer.push_back(frame.render());
  }

  // Update and add the frames
  for (int lyrics_index = 0; lyrics_index < frames_buffer.size();
       lyrics_index++) {
    TagLib::ID3v2::UnsynchronizedLyricsFrame* frame =
        new TagLib::ID3v2::UnsynchronizedLyricsFrame(
            frames_buffer.at(lyrics_index));
    if (lyrics_index == 0) {
      frame->setText(StdStringToTaglibString(value));
    }
    // add frame takes ownership and clears the memory
    tag->addFrame(frame);
  }
}

bool TagReader::IsMediaFile(const QString& filename) const {
  qLog(Debug) << "Checking for valid file" << filename;

  std::unique_ptr<TagLib::FileRef> fileref(factory_->GetFileRef(filename));
  return !fileref->isNull() && fileref->tag();
}

QByteArray TagReader::LoadEmbeddedArt(const QString& filename) const {
  if (filename.isEmpty()) return QByteArray();

  qLog(Debug) << "Loading art from" << filename;

#ifdef Q_OS_WIN32
  TagLib::FileRef ref(filename.toStdWString().c_str());
#else
  TagLib::FileRef ref(QFile::encodeName(filename).constData());
#endif

  if (ref.isNull() || !ref.file()) return QByteArray();

  // MP3
  TagLib::MPEG::File* file = dynamic_cast<TagLib::MPEG::File*>(ref.file());
  if (file && file->ID3v2Tag()) {
    TagLib::ID3v2::FrameList apic_frames =
        file->ID3v2Tag()->frameListMap()["APIC"];
    if (apic_frames.isEmpty()) return QByteArray();

    TagLib::ID3v2::AttachedPictureFrame* pic =
        static_cast<TagLib::ID3v2::AttachedPictureFrame*>(apic_frames.front());

    return QByteArray((const char*)pic->picture().data(),
                      pic->picture().size());
  }

  // Ogg vorbis/speex
  TagLib::Ogg::XiphComment* xiph_comment =
      dynamic_cast<TagLib::Ogg::XiphComment*>(ref.file()->tag());

  if (xiph_comment) {
    TagLib::Ogg::FieldListMap map = xiph_comment->fieldListMap();

#if TAGLIB_MAJOR_VERSION <= 1 && TAGLIB_MINOR_VERSION < 11
    // Other than the below mentioned non-standard COVERART,
    // METADATA_BLOCK_PICTURE
    // is the proposed tag for cover pictures.
    // (see http://wiki.xiph.org/VorbisComment#METADATA_BLOCK_PICTURE)
    if (map.contains("METADATA_BLOCK_PICTURE")) {
      TagLib::StringList pict_list = map["METADATA_BLOCK_PICTURE"];
      for (std::list<TagLib::String>::iterator it = pict_list.begin();
           it != pict_list.end(); ++it) {
        QByteArray data(QByteArray::fromBase64(it->toCString()));
        TagLib::ByteVector tdata(data.data(), data.size());
        TagLib::FLAC::Picture p(tdata);
        if (p.type() == TagLib::FLAC::Picture::FrontCover)
          return QByteArray(p.data().data(), p.data().size());
      }
      // If there was no specific front cover, just take the first picture
      QByteArray data(QByteArray::fromBase64(
          map["METADATA_BLOCK_PICTURE"].front().toCString()));
      TagLib::ByteVector tdata(data.data(), data.size());
      TagLib::FLAC::Picture p(tdata);
      return QByteArray(p.data().data(), p.data().size());
    }
#else
    TagLib::List<TagLib::FLAC::Picture*> pics = xiph_comment->pictureList();
    if (!pics.isEmpty()) {
      for (auto p : pics) {
        if (p->type() == TagLib::FLAC::Picture::FrontCover)
          return QByteArray(p->data().data(), p->data().size());
      }
      // If there was no specific front cover, just take the first picture
      std::list<TagLib::FLAC::Picture*>::iterator it = pics.begin();
      TagLib::FLAC::Picture* picture = *it;

      return QByteArray(picture->data().data(), picture->data().size());
    }
#endif

    // Ogg lacks a definitive standard for embedding cover art, but it seems
    // b64 encoding a field called COVERART is the general convention
    if (!map.contains("COVERART")) return QByteArray();

    return QByteArray::fromBase64(map["COVERART"].toString().toCString());
  }

#ifdef TAGLIB_HAS_FLAC_PICTURELIST
  // Flac
  TagLib::FLAC::File* flac_file = dynamic_cast<TagLib::FLAC::File*>(ref.file());
  if (flac_file && flac_file->xiphComment()) {
    TagLib::List<TagLib::FLAC::Picture*> pics = flac_file->pictureList();
    if (!pics.isEmpty()) {
      // Use the first picture in the file - this could be made cleverer and
      // pick the front cover if it's present.

      std::list<TagLib::FLAC::Picture*>::iterator it = pics.begin();
      TagLib::FLAC::Picture* picture = *it;

      return QByteArray(picture->data().data(), picture->data().size());
    }
  }
#endif

  // MP4/AAC
  TagLib::MP4::File* aac_file = dynamic_cast<TagLib::MP4::File*>(ref.file());
  if (aac_file) {
    TagLib::MP4::Tag* tag = aac_file->tag();
    TagLib::MP4::Item item = tag->item("covr");
    if (item.isValid()) {
      const TagLib::MP4::CoverArtList& art_list = item.toCoverArtList();

      if (!art_list.isEmpty()) {
        // Just take the first one for now
        const TagLib::MP4::CoverArt& art = art_list.front();
        return QByteArray(art.data().data(), art.data().size());
      }
    }
  }

  // APE formats
  auto apeTagCover = [&](TagLib::APE::Tag* tag) {
    QByteArray cover;
    const TagLib::APE::ItemListMap& items = tag->itemListMap();
    TagLib::APE::ItemListMap::ConstIterator it =
        items.find("COVER ART (FRONT)");
    if (it != items.end()) {
      TagLib::ByteVector data = it->second.binaryData();

      int pos = data.find('\0') + 1;
      if ((pos > 0) && (pos < data.size())) {
        cover = QByteArray(data.data() + pos, data.size() - pos);
      }
    }

    return cover;
  };

  TagLib::APE::File* ape_file = dynamic_cast<TagLib::APE::File*>(ref.file());
  if (ape_file) {
    return apeTagCover(ape_file->APETag());
  }

  TagLib::MPC::File* mpc_file = dynamic_cast<TagLib::MPC::File*>(ref.file());
  if (mpc_file) {
    return apeTagCover(mpc_file->APETag());
  }

  TagLib::WavPack::File* wavPack_file =
      dynamic_cast<TagLib::WavPack::File*>(ref.file());
  if (wavPack_file) {
    return apeTagCover(wavPack_file->APETag());
  }

  return QByteArray();
}

#ifdef HAVE_GOOGLE_DRIVE
bool TagReader::ReadCloudFile(const QUrl& download_url, const QString& title,
                              int size, const QString& mime_type,
                              const QString& authorisation_header,
                              cpb::tagreader::SongMetadata* song) const {
  qLog(Debug) << "Loading tags from" << title;

  std::unique_ptr<CloudStream> stream(
      new CloudStream(download_url, title, size, authorisation_header));
  stream->Precache();
  std::unique_ptr<TagLib::File> tag;
  if (mime_type == "audio/mpeg" &&
      title.endsWith(".mp3", Qt::CaseInsensitive)) {
    tag.reset(new TagLib::MPEG::File(stream.get(), true,
                                     TagLib::AudioProperties::Accurate,
                                     TagLib::ID3v2::FrameFactory::instance()));
  } else if (mime_type == "audio/mp4" ||
             (mime_type == "audio/mpeg" &&
              title.endsWith(".m4a", Qt::CaseInsensitive))) {
    tag.reset(new TagLib::MP4::File(stream.get(), true,
                                    TagLib::AudioProperties::Accurate));
  }
#ifdef TAGLIB_HAS_OPUS
  else if ((mime_type == "application/opus" || mime_type == "audio/opus" ||
            mime_type == "application/ogg" || mime_type == "audio/ogg") &&
           title.endsWith(".opus", Qt::CaseInsensitive)) {
    tag.reset(new TagLib::Ogg::Opus::File(stream.get(), true,
                                          TagLib::AudioProperties::Accurate));
  }
#endif
  else if (mime_type == "application/ogg" || mime_type == "audio/ogg") {
    tag.reset(new TagLib::Ogg::Vorbis::File(stream.get(), true,
                                            TagLib::AudioProperties::Accurate));
  } else if (mime_type == "application/x-flac" || mime_type == "audio/flac" ||
             mime_type == "audio/x-flac") {
    tag.reset(new TagLib::FLAC::File(stream.get(), true,
                                     TagLib::AudioProperties::Accurate,
                                     TagLib::ID3v2::FrameFactory::instance()));
  } else if (mime_type == "audio/x-ms-wma") {
    tag.reset(new TagLib::ASF::File(stream.get(), true,
                                    TagLib::AudioProperties::Accurate));
  } else {
    qLog(Debug) << "Unknown mime type for tagging:" << mime_type;
    return false;
  }

  if (stream->num_requests() > 2) {
    // Warn if pre-caching failed.
    qLog(Warning) << "Total requests for file:" << title
                  << stream->num_requests() << stream->cached_bytes();
  }

  if (tag->tag() && !tag->tag()->isEmpty()) {
    song->set_title(tag->tag()->title().toCString(true));
    song->set_artist(tag->tag()->artist().toCString(true));
    song->set_album(tag->tag()->album().toCString(true));
    song->set_filesize(size);

    if (tag->tag()->track() != 0) {
      song->set_track(tag->tag()->track());
    }
    if (tag->tag()->year() != 0) {
      song->set_year(tag->tag()->year());
    }

    song->set_type(cpb::tagreader::SongMetadata_Type_STREAM);

    if (tag->audioProperties()) {
      song->set_length_nanosec(tag->audioProperties()->lengthInMilliseconds() * kNsecPerMsec);
    }
    return true;
  }

  return false;
}
#endif  // HAVE_GOOGLE_DRIVE

TagLib::ID3v2::PopularimeterFrame* TagReader::GetPOPMFrameFromTag(
    TagLib::ID3v2::Tag* tag) {
  TagLib::ID3v2::PopularimeterFrame* frame = nullptr;

  const TagLib::ID3v2::FrameListMap& map = tag->frameListMap();
  if (!map["POPM"].isEmpty()) {
    frame =
        dynamic_cast<TagLib::ID3v2::PopularimeterFrame*>(map["POPM"].front());
  }

  if (!frame) {
    frame = new TagLib::ID3v2::PopularimeterFrame();
    tag->addFrame(frame);
  }
  return frame;
}

float TagReader::ConvertPOPMRating(const int POPM_rating) {
  if (POPM_rating < 0x01) {
    return 0.0;
  } else if (POPM_rating < 0x40) {
    return 0.20;  // 1 star
  } else if (POPM_rating < 0x80) {
    return 0.40;  // 2 stars
  } else if (POPM_rating < 0xC0) {
    return 0.60;                    // 3 stars
  } else if (POPM_rating < 0xFC) {  // some players store 5 stars as 0xFC
    return 0.80;                    // 4 stars
  }
  return 1.0;  // 5 stars
}

int TagReader::ConvertToPOPMRating(const float rating) {
  if (rating < 0.20) {
    return 0x00;
  } else if (rating < 0.40) {
    return 0x01;
  } else if (rating < 0.60) {
    return 0x40;
  } else if (rating < 0.80) {
    return 0x80;
  } else if (rating < 1.0) {
    return 0xC0;
  }
  return 0xFF;
}
