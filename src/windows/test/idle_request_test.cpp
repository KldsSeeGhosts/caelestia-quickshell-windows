#include <qtest.h>

#include "../wayland/idle_request.hpp"

using qs::windows::wayland::IdleRequestFns;
using qs::windows::wayland::IdleRequestState;
using qs::windows::wayland::applyIdleRequest;

namespace {

struct Calls {
	int sets = 0;
	int clears = 0;
	bool failDisplaySet = false;
	bool failSystemSet = false;
	bool failDisplayClear = false;
	bool failSystemClear = false;
};

Calls* active = nullptr;

BOOL fakeSet(HANDLE, POWER_REQUEST_TYPE type) {
	active->sets++;
	if (type == PowerRequestDisplayRequired && active->failDisplaySet) return FALSE;
	if (type == PowerRequestSystemRequired && active->failSystemSet) return FALSE;
	return TRUE;
}

BOOL fakeClear(HANDLE, POWER_REQUEST_TYPE type) {
	active->clears++;
	if (type == PowerRequestDisplayRequired && active->failDisplayClear) return FALSE;
	if (type == PowerRequestSystemRequired && active->failSystemClear) return FALSE;
	return TRUE;
}

IdleRequestFns fns() { return {fakeSet, fakeClear}; }

}

class IdleRequestTest: public QObject {
	Q_OBJECT;

private slots:
	void init() {
		active = &this->calls;
		this->calls = {};
		this->state = {};
	}

	void acquire_sets_each_type_once() {
		QVERIFY(applyIdleRequest(nullptr, true, this->state, fns()));
		QCOMPARE(this->calls.sets, 2);
		QCOMPARE(this->calls.clears, 0);
		QVERIFY(this->state.fullyHeld());
		QVERIFY(applyIdleRequest(nullptr, true, this->state, fns()));
		QCOMPARE(this->calls.sets, 2);
	}

	void failed_display_set_holds_nothing() {
		this->calls.failDisplaySet = true;
		QVERIFY(!applyIdleRequest(nullptr, true, this->state, fns()));
		QCOMPARE(this->calls.sets, 1);
		QCOMPARE(this->calls.clears, 0);
		QVERIFY(!this->state.held());
	}

	void partial_acquire_rolls_back() {
		this->calls.failSystemSet = true;
		QVERIFY(!applyIdleRequest(nullptr, true, this->state, fns()));
		QCOMPARE(this->calls.sets, 2);
		QCOMPARE(this->calls.clears, 1);
		QVERIFY(!this->state.held());
	}

	void failed_rollback_keeps_the_acquired_type() {
		this->calls.failSystemSet = true;
		this->calls.failDisplayClear = true;
		QVERIFY(!applyIdleRequest(nullptr, true, this->state, fns()));
		QVERIFY(this->state.display);
		QVERIFY(!this->state.system);
		QVERIFY(this->state.held());
	}

	void release_clears_both() {
		QVERIFY(applyIdleRequest(nullptr, true, this->state, fns()));
		QVERIFY(applyIdleRequest(nullptr, false, this->state, fns()));
		QCOMPARE(this->calls.clears, 2);
		QVERIFY(!this->state.held());
	}

	void failed_clear_stays_held_and_retry_does_not_reset() {
		QVERIFY(applyIdleRequest(nullptr, true, this->state, fns()));
		this->calls.failSystemClear = true;
		QVERIFY(!applyIdleRequest(nullptr, false, this->state, fns()));
		QVERIFY(!this->state.display);
		QVERIFY(this->state.system);
		const int sets = this->calls.sets;
		QVERIFY(applyIdleRequest(nullptr, true, this->state, fns()));
		QCOMPARE(this->calls.sets, sets + 1);
		QVERIFY(this->state.fullyHeld());
	}

private:
	Calls calls;
	IdleRequestState state;
};

QTEST_GUILESS_MAIN(IdleRequestTest);
#include "idle_request_test.moc"
