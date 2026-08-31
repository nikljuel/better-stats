#include "stats_model.h"
#include "tracker.h"
#include "miniz.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void sql(sqlite3 *db, const char *statement)
{
    char *error = NULL;
    if (sqlite3_exec(db, statement, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "%s\n%s\n", error ? error : "SQL failed", statement);
        exit(1);
    }
}

static void make_epub(const char *path)
{
    static const char container[] =
        "<container><rootfiles><rootfile full-path='OPS/content.opf'/>"
        "</rootfiles></container>";
    static const char opf[] =
        "<package><manifest><item id='cover' href='../images/cover.png'"
        " media-type='image/png' properties='cover-image'/></manifest></package>";
    static const unsigned char png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
        0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,
        0x0d,0x49,0x44,0x41,0x54,0x08,0xd7,0x63,0xf8,0xcf,0xc0,0xf0,
        0x1f,0x00,0x05,0x00,0x01,0xff,0x89,0x99,0x3d,0x1d,0x00,0x00,
        0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
    };
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    assert(mz_zip_writer_init_file(&zip, path, 0));
    assert(mz_zip_writer_add_mem(&zip, "META-INF/container.xml", container,
                                 sizeof(container) - 1, MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_add_mem(&zip, "OPS/content.opf", opf,
                                 sizeof(opf) - 1, MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_add_mem(&zip, "images/cover.png", png, sizeof(png),
                                 MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&zip));
    assert(mz_zip_writer_end(&zip));
}

static void make_fb2(const char *path)
{
    static const char fb2[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\""
        " xmlns:l=\"http://www.w3.org/1999/xlink\">\n"
        "<description><title-info><coverpage>"
        "<image l:href=\"#cover.png\"/>"
        "</coverpage></title-info></description>\n"
        "<binary id=\"cover.png\" content-type=\"image/png\">"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQI12P4"
        "z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="
        "</binary>\n"
        "</FictionBook>\n";
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(fb2, 1, sizeof(fb2) - 1, f) == sizeof(fb2) - 1);
    assert(fclose(f) == 0);
}

static void make_cbz(const char *path)
{
    static const char metadata[] = "<ComicInfo/>";
    static const unsigned char png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
        0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,
        0x0d,0x49,0x44,0x41,0x54,0x08,0xd7,0x63,0xf8,0xcf,0xc0,0xf0,
        0x1f,0x00,0x05,0x00,0x01,0xff,0x89,0x99,0x3d,0x1d,0x00,0x00,
        0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
    };
    static const unsigned char jpg[] = {0xff, 0xd8, 0xff, 0xd9};
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    assert(mz_zip_writer_init_file(&zip, path, 0));
    assert(mz_zip_writer_add_mem(&zip, "ComicInfo.xml", metadata,
                                 sizeof(metadata) - 1, MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_add_mem(&zip, "002.jpg", jpg, sizeof(jpg),
                                 MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_add_mem(&zip, "001.png", png, sizeof(png),
                                 MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&zip));
    assert(mz_zip_writer_end(&zip));
}

int main(void)
{
    char explorer_path[128];
    char stats_path[128];
    char epub_path[128];
    char epub_name[64];
    char fb2_path[128];
    char fb2_name[64];
    char cbz_path[128];
    char cbz_name[64];
    snprintf(explorer_path, sizeof(explorer_path), "/tmp/bs_model_%ld_explorer.db",
             (long)getpid());
    snprintf(stats_path, sizeof(stats_path), "/tmp/bs_model_%ld_stats.db",
             (long)getpid());
    snprintf(epub_name, sizeof(epub_name), "bs_model_%ld.epub", (long)getpid());
    snprintf(epub_path, sizeof(epub_path), "/tmp/%s", epub_name);
    snprintf(fb2_name, sizeof(fb2_name), "bs_model_%ld.fb2", (long)getpid());
    snprintf(fb2_path, sizeof(fb2_path), "/tmp/%s", fb2_name);
    snprintf(cbz_name, sizeof(cbz_name), "bs_model_%ld.cbz", (long)getpid());
    snprintf(cbz_path, sizeof(cbz_path), "/tmp/%s", cbz_name);
    unlink(explorer_path);
    unlink(stats_path);
    unlink(epub_path);
    unlink(fb2_path);
    unlink(cbz_path);
    unlink("/tmp/bs_model_cache/covers/aa.png");
    unlink("/tmp/bs_model_cache/covers/dd.png");
    unlink("/tmp/bs_model_cache/covers/ee.png");
    unlink("/tmp/bs_model_cache/covers/ee.jpg");
    rmdir("/tmp/bs_model_cache/covers");
    unlink("/tmp/bs_model_cache/cbz-cover-fix");
    rmdir("/tmp/bs_model_cache");
    unlink("/tmp/bs_model_firmware_covers/1bb.png");
    rmdir("/tmp/bs_model_firmware_covers");
    assert(mkdir("/tmp/bs_model_cache", 0700) == 0);
    assert(mkdir("/tmp/bs_model_firmware_covers", 0700) == 0);
    setenv("TZ", "UTC", 1);
    tzset();

    sqlite3 *explorer = NULL;
    assert(sqlite3_open(explorer_path, &explorer) == SQLITE_OK);
    sql(explorer,
        "CREATE TABLE books_impl(id INTEGER PRIMARY KEY,title TEXT,author TEXT);"
        "CREATE TABLE folders(id INTEGER PRIMARY KEY,name TEXT);"
        "CREATE TABLE files(book_id INTEGER,storageid INTEGER,fast_hash BLOB,"
        " folder_id INTEGER,filename TEXT,modification_time INTEGER);"
        "CREATE TABLE books_settings(bookid INTEGER,profileid INTEGER,position TEXT,"
        " position_ts INTEGER,cpage INTEGER,npage INTEGER,opentime INTEGER,"
        " completed INTEGER,completed_ts INTEGER);"
        "INSERT INTO folders VALUES(1,'/missing');"
        "INSERT INTO books_impl VALUES(1,'Alpha','Ada'),(2,'Beta','Bea'),"
        " (3,'Gamma','Gina');"
        "INSERT INTO files VALUES(1,1,x'aa',1,'alpha.epub',2),"
        " (2,1,x'bb',1,'beta.epub',2),"
        " (2,1,x'cc',1,'beta-old.epub',1),"
        " (3,1,x'dd',1,'gamma.fb2',3);"
        "INSERT INTO books_settings VALUES"
        " (1,1,'p',1700000060,25,100,1700000000,0,0),"
        " (2,1,'p',0,100,100,0,1,strftime('%s','2024-02-29 18:00:00')),"
        " (3,1,'p',1699999900,200,200,1699999800,1,strftime('%s','2024-01-15 10:00:00'));"
    );
    char update[512];
    snprintf(update, sizeof(update),
             "UPDATE folders SET name='/tmp';UPDATE files SET filename='%s'"
             " WHERE book_id=1;UPDATE files SET filename='%s'"
             " WHERE book_id=3", epub_name, fb2_name);
    sql(explorer, update);
    sqlite3_close(explorer);
    make_epub(epub_path);
    make_fb2(fb2_path);
    make_cbz(cbz_path);
    FILE *firmware_cover = fopen("/tmp/bs_model_firmware_covers/1bb.png", "wb");
    assert(firmware_cover);
    assert(fputs("cover", firmware_cover) >= 0);
    assert(fclose(firmware_cover) == 0);

    tracker setup;
    assert(tracker_init(&setup, stats_path, explorer_path) == 0);
    sql(setup.stats,
        "INSERT OR REPLACE INTO books"
        " (book_id,title,author,cover,cpage,npage,completed,completed_ts,last_seen)"
        " VALUES(1,'Alpha','Ada','1aa',25,100,0,0,0),"
        " (2,'Beta','Bea','1bb',100,100,1,"
        "  strftime('%s','2024-02-29 18:00:00'),0),"
        " (3,'Gamma','Gina','1dd',200,200,1,"
        "  strftime('%s','2024-01-15 10:00:00'),0);"
        "INSERT INTO sessions(book_id,start_time,end_time,active_seconds,pages_start,"
        " pages_end,recovered) VALUES"
        " (1,strftime('%s','2024-02-28 11:50:00'),strftime('%s','2024-02-28 12:00:00'),"
        " 600,10,20,0),"
        " (2,strftime('%s','2024-02-29 17:58:00'),strftime('%s','2024-02-29 18:00:00'),"
        " 120,98,100,0);"
    );
    tracker_close(&setup);

    bs_error error;
    bs_context *context = NULL;
    assert(bs_context_open(&context, stats_path, explorer_path, &error) == 0);

    bs_overall overall;
    assert(bs_load_overall(context, &overall, &error) == 0);
    assert(overall.books_total == 3 && overall.books_finished == 2);

    bs_current_book current;
    assert(bs_load_current_book(context, &current, &error) == 0);
    assert(current.ok && current.percent == 25);
    assert(strcmp(current.title, "Alpha") == 0);
    assert(strcmp(current.author, "Ada") == 0);
    /* Our own cache is keyed by the hash alone, without the storage id: the
     * same book is indexed once per storage, so a key carrying it would give
     * one book two names and orphan the cached image whenever the row order
     * flips. The firmware's own cache below keeps the storage id, because the
     * firmware picks that name. */
    assert(strcmp(current.cover_path, "/tmp/bs_model_cache/covers/aa.png") == 0);
    assert(strstr(current.cover_path, "/1aa.png") == NULL);
    assert(access(current.cover_path, R_OK) == 0);

    /* CBZ metadata is not a page; use the first image in archive order. */
    assert(sqlite3_open(explorer_path, &explorer) == SQLITE_OK);
    snprintf(update, sizeof(update),
             "UPDATE files SET fast_hash=x'ee',filename='%s' WHERE book_id=1",
             cbz_name);
    sql(explorer, update);
    sqlite3_close(explorer);
    assert(bs_load_current_book(context, &current, &error) == 0);
    assert(strcmp(current.cover_path, "/tmp/bs_model_cache/covers/ee.png") == 0);
    assert(access(current.cover_path, R_OK) == 0);
    FILE *broken_cbz = fopen(cbz_path, "wb");
    assert(broken_cbz);
    assert(fputs("not a zip", broken_cbz) >= 0);
    assert(fclose(broken_cbz) == 0);
    assert(bs_load_current_book(context, &current, &error) == 0);
    assert(strcmp(current.cover_path, "/tmp/bs_model_cache/covers/ee.png") == 0);
    assert(unlink(current.cover_path) == 0);
    assert(bs_load_current_book(context, &current, &error) == 0);
    assert(current.cover_path[0] == '\0');

    bs_year year;
    assert(bs_load_year(context, 2024, &year, &error) == 0);
    assert(year.days == 366 && year.start_weekday == 0);
    assert(year.days_read == 3 && year.best_streak == 2);
    assert(strcmp(year.best_streak_start, "2024-02-28") == 0);

    bs_month month;
    assert(bs_load_month(context, 2024, 2, &month, &error) == 0);
    assert(month.days == 29 && month.first_weekday == 3);
    assert(month.day[27].seconds == 600 && month.day[27].book_count == 1);
    assert(month.day[28].seconds == 120 && month.day[28].book_count == 1);
    bs_month_free(&month);

    bs_year_books books;
    assert(bs_load_year_books(context, 2024, &books, &error) == 0);
    assert(books.total == 2);
    assert(books.month_count[0] == 1);
    assert(strcmp(books.month[0][0].title, "Gamma") == 0);
    assert(strcmp(books.month[0][0].date, "2024-01-15") == 0);
    assert(books.month_count[1] == 1);
    assert(strcmp(books.month[1][0].title, "Beta") == 0);
    assert(strcmp(books.month[1][0].date, "2024-02-29") == 0);
    assert(strcmp(books.month[1][0].cover_path,
                  "/tmp/bs_model_firmware_covers/1bb.png") == 0);
    /* FB2 cover was extracted via base64 decode */
    assert(access("/tmp/bs_model_cache/covers/dd.png", R_OK) == 0);
    bs_year_books_free(&books);

    /* Returning a loan can remove every firmware row for it. Exact completion
     * dates, calendar sessions and remembered covers must survive locally. */
    assert(sqlite3_open(explorer_path, &explorer) == SQLITE_OK);
    sql(explorer,
        "DELETE FROM files WHERE book_id IN (2,3);"
        "DELETE FROM books_settings WHERE bookid IN (2,3);"
        "DELETE FROM books_impl WHERE id IN (2,3)");
    sqlite3_close(explorer);

    assert(bs_load_overall(context, &overall, &error) == 0);
    assert(overall.books_finished == 2);
    assert(bs_load_year(context, 2024, &year, &error) == 0);
    assert(year.heat[14] == 2 && year.heat[59] == 2);
    assert(bs_load_month(context, 2024, 2, &month, &error) == 0);
    assert(month.day[28].book_count == 1);
    assert(strcmp(month.day[28].books[0].title, "Beta") == 0);
    assert(strcmp(month.day[28].books[0].cover_path,
                  "/tmp/bs_model_firmware_covers/1bb.png") == 0);
    bs_month_free(&month);
    assert(bs_load_year_books(context, 2024, &books, &error) == 0);
    assert(books.total == 2);
    assert(strcmp(books.month[0][0].cover_path,
                  "/tmp/bs_model_cache/covers/dd.png") == 0);
    assert(strcmp(books.month[1][0].cover_path,
                  "/tmp/bs_model_firmware_covers/1bb.png") == 0);
    bs_year_books_free(&books);

    bs_context_close(context);
    unlink("/tmp/bs_model_cache/covers/aa.png");
    unlink("/tmp/bs_model_cache/covers/dd.png");
    unlink("/tmp/bs_model_cache/covers/ee.png");
    unlink("/tmp/bs_model_cache/covers/ee.jpg");
    rmdir("/tmp/bs_model_cache/covers");
    unlink("/tmp/bs_model_cache/cbz-cover-fix");
    rmdir("/tmp/bs_model_cache");
    unlink("/tmp/bs_model_firmware_covers/1bb.png");
    rmdir("/tmp/bs_model_firmware_covers");
    unlink(epub_path);
    unlink(fb2_path);
    unlink(cbz_path);
    unlink(explorer_path);
    unlink(stats_path);
    puts("all stats-model tests ok");
    return 0;
}
