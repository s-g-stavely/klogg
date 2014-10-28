#include "gmock/gmock.h"

#include "config.h"

#include <cstdio>
#include <fcntl.h>

#include <memory>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <chrono>

#include "log.h"

#include "watchtower.h"

#ifdef _WIN32
#  include "winwatchtowerdriver.h"
using PlatformWatchTower = WatchTower<WinWatchTowerDriver>;
#else
#  include "inotifywatchtowerdriver.h"
using PlatformWatchTower = WatchTower<INotifyWatchTowerDriver>;
#endif

using namespace std;
using namespace testing;

class WatchTowerBehaviour: public testing::Test {
  public:
    shared_ptr<PlatformWatchTower> watch_tower = make_shared<PlatformWatchTower>();

    const char* createTempName()
    {
        const char* name;
#if _WIN32
        name = _tempnam( "c:\\temp", "glogg_test" );
#else
        name = tmpnam( nullptr );
#endif
        return name;
    }

    string createTempEmptyFile( string file_name = "" ) {
        const char* name;

        if ( ! file_name.empty() ) {
            name = file_name.c_str();
        }
        else {
            // I know tmpnam is bad but I need control over the file
            // and it is the only one which exits on Windows.
            name = createTempName();
        }
        int fd = creat( name, S_IRUSR | S_IWUSR );
        close( fd );

        return string( name );
    }

    string getNonExistingFileName() {
#if _WIN32
        return string( _tempnam( "c:\\temp", "inexistant" ) );
#else
        return string( tmpnam( nullptr ) );
#endif
    }

    WatchTowerBehaviour() {
        // Default to quiet, but increase to debug
        FILELog::setReportingLevel( logERROR );
    }
};

TEST_F( WatchTowerBehaviour, AcceptsAnExistingFileToWatch ) {
    auto file_name = createTempEmptyFile();
    auto registration = watch_tower->addFile( file_name, [] (void) { } );
}

TEST_F( WatchTowerBehaviour, AcceptsANonExistingFileToWatch ) {
    auto registration = watch_tower->addFile( getNonExistingFileName(), [] (void) { } );
}

/*****/

class WatchTowerSingleFile: public WatchTowerBehaviour {
  public:
    static const int TIMEOUT;

    string file_name;
    Registration registration;

    mutex mutex_;
    condition_variable cv_;
    int notification_received = 0;

    Registration registerFile( const string& filename ) {
        weak_ptr<void> weakHeartbeat( heartbeat_ );

        auto reg = watch_tower->addFile( filename, [this, weakHeartbeat] (void) {
            // Ensure the fixture object is still alive using the heartbeat
            if ( auto keep = weakHeartbeat.lock() ) {
                unique_lock<mutex> lock(mutex_);
                ++notification_received;
                cv_.notify_one();
            } } );

        return reg;
    }

    WatchTowerSingleFile()
        : heartbeat_( shared_ptr<void>( (void*) 0xDEADC0DE, [] (void*) {} ) )
    {
        file_name = createTempEmptyFile();
        registration = registerFile( file_name );
    }

    ~WatchTowerSingleFile() {
        remove( file_name.c_str() );
    }

    bool waitNotificationReceived( int number = 1 ) {
        unique_lock<mutex> lock(mutex_);
        bool result = ( cv_.wait_for( lock, std::chrono::milliseconds(TIMEOUT),
                [this, number] { return notification_received >= number; } ) );

        // Reinit the notification
        notification_received = 0;

        return result;
    }

    void appendDataToFile( const string& file_name ) {
        static const char* string = "Test line\n";
        int fd = open( file_name.c_str(), O_WRONLY | O_APPEND );
        write( fd, (void*) string, strlen( string ) );
        close( fd );
    }

  private:
    // Heartbeat ensures the object is still alive
    shared_ptr<void> heartbeat_;
};

#ifdef _WIN32
const int WatchTowerSingleFile::TIMEOUT = 1000;
#else
const int WatchTowerSingleFile::TIMEOUT = 20;
#endif

TEST_F( WatchTowerSingleFile, SignalsWhenAWatchedFileIsAppended ) {
    appendDataToFile( file_name );
    ASSERT_TRUE( waitNotificationReceived() );
}

TEST_F( WatchTowerSingleFile, SignalsWhenAWatchedFileIsRemoved) {
    remove( file_name.c_str() );
    ASSERT_TRUE( waitNotificationReceived() );
}

TEST_F( WatchTowerSingleFile, SignalsWhenADeletedFileReappears ) {
    remove( file_name.c_str() );
    waitNotificationReceived();
    createTempEmptyFile( file_name );
    ASSERT_TRUE( waitNotificationReceived() );
}

TEST_F( WatchTowerSingleFile, StopSignalingWhenWatchDeleted ) {
    auto second_file_name = createTempEmptyFile();
    {
        auto second_registration = registerFile( second_file_name );
        appendDataToFile( second_file_name );
        ASSERT_TRUE( waitNotificationReceived() );
    }

    // The registration will be removed here.
    appendDataToFile( second_file_name );
    ASSERT_FALSE( waitNotificationReceived() );

    remove( second_file_name.c_str() );
}

TEST_F( WatchTowerSingleFile, SignalsWhenSameFileIsFollowedMultipleTimes ) {
    auto second_file_name = createTempEmptyFile();

    for ( int i = 0; i < 1000; i++ )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        auto second_registration = registerFile( second_file_name );
        appendDataToFile( second_file_name );
        ASSERT_TRUE( waitNotificationReceived() );
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        appendDataToFile( second_file_name );
    }

    // The registration will be removed here.
    appendDataToFile( second_file_name );
    ASSERT_FALSE( waitNotificationReceived() );

    remove( second_file_name.c_str() );
}

TEST_F( WatchTowerSingleFile, TwoWatchesOnSameFileYieldsTwoNotifications ) {
    auto second_registration = registerFile( file_name );
    appendDataToFile( file_name );

    ASSERT_TRUE( waitNotificationReceived( 2 ) );
}

TEST_F( WatchTowerSingleFile, RemovingOneWatchOfTwoStillYieldsOneNotification ) {
    {
        auto second_registration = registerFile( file_name );
    }

    appendDataToFile( file_name );
    ASSERT_TRUE( waitNotificationReceived( 1 ) );
}

TEST_F( WatchTowerSingleFile, RenamingTheFileYieldsANotification ) {
    auto new_file_name = createTempName();

    rename( file_name.c_str(), new_file_name );
    ASSERT_TRUE( waitNotificationReceived() );

    rename( new_file_name, file_name.c_str() );
}

TEST_F( WatchTowerSingleFile, RenamingAFileToTheWatchedNameYieldsANotification ) {
    remove( file_name.c_str() );
    waitNotificationReceived();

    std::string new_file_name = createTempEmptyFile();
    appendDataToFile( new_file_name );

    rename( new_file_name.c_str(), file_name.c_str() );
    ASSERT_TRUE( waitNotificationReceived() );
}

/*****/

#ifdef HAVE_SYMLINK
class WatchTowerSymlink: public WatchTowerSingleFile {
  public:
    string symlink_name;

    void SetUp() override {
        file_name = createTempEmptyFile();
        symlink_name = createTempEmptyFile();
        remove( symlink_name.c_str() );
        symlink( file_name.c_str(), symlink_name.c_str() );

        registration = registerFile( symlink_name );
    }

    void TearDown() override {
        remove( symlink_name.c_str() );
        remove( file_name.c_str() );
    }
};

TEST_F( WatchTowerSymlink, AppendingToTheSymlinkYieldsANotification ) {
    appendDataToFile( symlink_name );
    ASSERT_TRUE( waitNotificationReceived() );
}

TEST_F( WatchTowerSymlink, AppendingToTheTargetYieldsANotification ) {
    appendDataToFile( file_name );
    ASSERT_TRUE( waitNotificationReceived() );
}

TEST_F( WatchTowerSymlink, RemovingTheSymlinkYieldsANotification ) {
    remove( symlink_name.c_str() );
    ASSERT_TRUE( waitNotificationReceived() );
}

TEST_F( WatchTowerSymlink, RemovingTheTargetYieldsANotification ) {
    remove( file_name.c_str() );
    ASSERT_TRUE( waitNotificationReceived() );
}

TEST_F( WatchTowerSymlink, ReappearingSymlinkYieldsANotification ) {
    auto new_target = createTempEmptyFile();
    remove( symlink_name.c_str() );
    waitNotificationReceived();

    symlink( new_target.c_str(), symlink_name.c_str() );
    ASSERT_TRUE( waitNotificationReceived() );

    remove( new_target.c_str() );
}
#endif //HAVE_SYMLINK

/*****/

TEST( WatchTowerLifetime, RegistrationCanBeDeletedWhenWeAreDead ) {
    auto mortal_watch_tower = new PlatformWatchTower();
    auto reg = mortal_watch_tower->addFile( "/tmp/test_file", [] (void) { } );

    delete mortal_watch_tower;
    // reg will be destroyed after the watch_tower
}

/*****/

#ifdef _WIN32
class WinNotificationInfoListTest : public testing::Test {
  public:
    using Action = WinNotificationInfo::Action;

    struct Buffer {
        uint32_t next_addr;
        uint32_t action;
        uint32_t filename_length;
        wchar_t filename[13];
    };
    static struct Buffer buffer[2];

    WinNotificationInfoList list { reinterpret_cast<char*>( buffer ), sizeof( buffer ) };
    WinNotificationInfoList::iterator iterator { std::begin( list ) };
};

struct WinNotificationInfoListTest::Buffer WinNotificationInfoListTest::buffer[] =
    { { 40, 1, 26, L"Filename.txt" },
      { 0, 3, 18, L"file2.txt" } };

TEST_F( WinNotificationInfoListTest, FirstNotificationCanBeObtained ) {
    auto notification = *iterator;
    ASSERT_THAT( &notification, NotNull() );
}

TEST_F( WinNotificationInfoListTest, FirstNotificationHasRightAction ) {
    ASSERT_THAT( (*iterator).action(), Eq( Action::ADDED ) );
}

TEST_F( WinNotificationInfoListTest, FirstNotificationHasRightFileName ) {
    ASSERT_THAT( (*iterator).fileName(), Eq( std::wstring( L"Filename.txt\0", 13 ) ) );
}

TEST_F( WinNotificationInfoListTest, SecondNotificationCanBeObtained ) {
    ++iterator;
    auto notification = *iterator;
    ASSERT_THAT( &notification, NotNull() );
}

TEST_F( WinNotificationInfoListTest, SecondNotificationIsCorrect ) {
    iterator++;
    ASSERT_THAT( iterator->action(), Eq( Action::MODIFIED ) );
    ASSERT_THAT( iterator->fileName(), Eq( L"file2.txt" ) );
}

TEST_F( WinNotificationInfoListTest, CanBeIteratedByFor ) {
    for ( auto notification : list ) {
        notification.action();
    }
}
#endif
