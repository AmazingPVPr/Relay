#include "GifSearch.h"
#include <QtTest>
#include <QTemporaryDir>
class GifSearchTest final:public QObject{
 Q_OBJECT
private slots:
 void validatesAndPersistsFavorites(){
  QTemporaryDir dir;const auto file=dir.path()+"/prefs.ini";
  const QVariantMap input{{"id","hi"},{"title","Hello"},{"url","https://klipy.com/gifs/hello"},{"preview","https://static.klipy.com/test.gif"},{"width",498},{"height",498}};
  const auto record=GifSearch::validatedRecord(input);QVERIFY(!record.isEmpty());
  {GifSearch gifs(file);QVERIFY(!gifs.busy());gifs.setFavorite(record,true);QCOMPARE(gifs.favorites().size(),1);gifs.setFavorite(record,true);QCOMPARE(gifs.favorites().size(),1);}
  GifSearch restored(file);QVERIFY(restored.isFavorite(record.value("url").toString()));restored.showFavorites();QCOMPARE(restored.results().size(),1);
  auto invalid=input;invalid["url"]="https://klipy.com.evil.test/gifs/hello";QVERIFY(GifSearch::validatedRecord(invalid).isEmpty());
  invalid=input;invalid["preview"]="file:///tmp/private.png";QVERIFY(GifSearch::validatedRecord(invalid).isEmpty());
  QVERIFY(!GifSearch::allowedMedia("https://user@static.klipy.com/file.gif"));QVERIFY(!GifSearch::allowedMedia("https://static.klipy.com:123/file.gif"));
  restored.setFavorite(record,false);QVERIFY(restored.favorites().isEmpty());
 }
};
QTEST_GUILESS_MAIN(GifSearchTest)
#include "gif_search_test.moc"
