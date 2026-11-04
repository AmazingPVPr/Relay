#include <QtTest>
#include "BroadcastPacer.h"
#include "BroadcastRepairBudget.h"
#include <array>

class BroadcastPacerTest final : public QObject {
    Q_OBJECT
private slots:
    void repairBudgetFollowsActualOriginalRateAndAllowsQuietIdrRecovery() {
        BroadcastRepairBudget budget;budget.update(0,40000000);
        int sent=0;for(int ms=1;ms<=4000;++ms){
            // 8 Mb/s actual source, even though Source quality permits40.
            budget.originalSent(ms*1000,1000);budget.update(ms*1000,40000000);
            while(budget.ready(1180)){budget.sent(1180);++sent;}
        }
        // Actual-rate allowance is <=25% plus the two-packet initial burst.
        QVERIFY(sent*1180<=1000000+2400);QVERIFY(sent*1180>700000);
        BroadcastRepairBudget quiet;quiet.update(0,24000000);quiet.originalSent(0,300000);
        int repaired=0;for(int ms=1;ms<=500;++ms){quiet.update(ms*1000,24000000);while(quiet.ready(1180)){quiet.sent(1180);++repaired;}}
        QVERIFY(repaired>=30); // A large IDR's credit survives tiny/no P frames.
        for(int ms=501;ms<=1100;++ms)quiet.update(ms*1000,24000000);
        while(quiet.ready(1180))quiet.sent(1180);
        for(int ms=1101;ms<=5000;++ms)quiet.update(ms*1000,24000000);
        QVERIFY(!quiet.ready(1180)); // Idle does not mint unlimited replays.
    }
    void repairBudgetCapsConfiguredRateAndLowRateStillProgresses() {
        BroadcastRepairBudget budget;budget.update(0,8000000);int repaired=0;
        for(int ms=1;ms<=1000;++ms){budget.originalSent(ms*1000,10000);budget.update(ms*1000,8000000);while(budget.ready(1180)){budget.sent(1180);++repaired;}}
        QVERIFY(repaired*1180<=150000+2400);QVERIFY(repaired*1180>130000);
        BroadcastRepairBudget low;low.update(0,64000);int lowRepairs=0;
        for(int ms=1;ms<=3000;++ms){if(ms%20==0)low.originalSent(ms*1000,160);low.update(ms*1000,64000);if(low.ready(1180)){low.sent(1180);++lowRepairs;}}
        QVERIFY(lowRepairs>=4);QVERIFY(lowRepairs<=6);
        low.reset();low.update(4000000,64000);QVERIFY(low.ready(1180));low.sent(1180);QVERIFY(low.ready(1180));low.sent(1180);QVERIFY(!low.ready(1180));
    }
    void largeKeyframeSurvivesBoundedLink() {
        BroadcastPacer pacer;
        pacer.update(0,24000000,false);
        int remaining=307,sent=0,dropped=0;
        double linkCredit=8*1180;
        int completedAt=0;
        // Independent 40 Mb/s receiver path with an eight-datagram queue.
        // The former 32-packet timer burst overflows this generated path.
        for(int ms=1;ms<1000 && remaining;++ms){
            linkCredit=std::min(8*1180.,linkCredit+40000000./8/1000);
            pacer.update(ms*1000,24000000,false);
            int batch=0;
            while(batch<BroadcastPacer::packetsPerWakeup && remaining && pacer.ready(1180)){
                pacer.sent(1180);++batch;++sent;--remaining;
                if(linkCredit<1180)++dropped;else linkCredit-=1180;
            }
            QVERIFY(batch<=8);completedAt=ms;
        }
        QCOMPARE(sent,307);QCOMPARE(dropped,0);
        // 307 * 1180 bytes at a 30 Mb/s wire budget needs about 97 ms.
        QVERIFY(completedAt>=96);QVERIFY(completedAt<=99);
    }
    void boundsCatchupAfterStallAndResets() {
        BroadcastPacer pacer;pacer.update(0,64000000,false);
        pacer.update(10000000,64000000,false);
        int packets=0;
        while(pacer.ready(1200)){pacer.sent(1200);++packets;}
        QCOMPARE(packets,8);
        // No unbounded leftover credit after a ten-second UI stall.
        QVERIFY(!pacer.ready(1));
        pacer.reset();pacer.update(11000000,64000000,false);
        QVERIFY(!pacer.ready(1));
        pacer.update(11001000,64000000,false);QVERIFY(pacer.ready(1200));
    }
    void lowRateMakesProgressAndAudioCannotStarveVideo() {
        BroadcastPacer pacer;pacer.update(0,64000,true);
        int videoPackets=0,audioPackets=0;
        for(int ms=1;ms<=2000;++ms){
            pacer.update(ms*1000,64000,true);
            if(ms%20==0){pacer.sent(680);++audioPackets;}
            if(pacer.ready(1180)){pacer.sent(1180);++videoPackets;}
        }
        QCOMPARE(audioPackets,100);
        // Real 256 kb/s audio (plus headers) still leaves room for video.
        QVERIFY(videoPackets>=20);QVERIFY(videoPackets<=28);
        BroadcastPacer verySlow;verySlow.update(0,8000,false);
        for(int ms=1;ms<=1000;++ms)verySlow.update(ms*1000,8000,false);
        QVERIFY(verySlow.ready(1200));
    }
    void budgetUsesActualPacketBytesAndNeverMintsTime() {
        BroadcastPacer pacer;pacer.update(0,8000000,false);
        pacer.update(1000,8000000,false); // 1250 bytes available
        QVERIFY(pacer.ready(1200));pacer.sent(1200);
        QVERIFY(pacer.ready(50));QVERIFY(!pacer.ready(51));
        pacer.update(1000,8000000,false);QVERIFY(!pacer.ready(51));
        QVERIFY(!pacer.ready(0));QVERIFY(!pacer.ready(-1));
        QVERIFY(!pacer.ready(BroadcastPacer::maximumCredit+1));
    }
};
QTEST_GUILESS_MAIN(BroadcastPacerTest)
#include "broadcast_pacer_test.moc"
