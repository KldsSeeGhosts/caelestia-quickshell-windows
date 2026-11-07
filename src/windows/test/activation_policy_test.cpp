// The activation policy: the pure foreground-handoff decision, the source gate
// that allows the accepted hook escalation, and a source guard over the
// activation path. No window is created, no foreground change happens and no
// input is sent.

#error INTENTIONAL_CI_COMPILE_FAILURE_CONTROL_DO_NOT_MERGE
#include <array>

#include <qdir.h>
#include <qfile.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtest.h>

#include "../activation_policy.hpp"
#include "../foreground_policy.hpp"

using qs::windows::ActivationDispatchScope;
using qs::windows::ActivationSource;
using qs::windows::ForegroundAction;
using qs::windows::ForegroundHandoff;
using qs::windows::PendingActivation;
using qs::windows::RememberedForeground;
using qs::windows::currentActivationSource;
using qs::windows::foregroundHandoff;
using qs::windows::mayForceForeground;
using qs::windows::shouldRestoreRememberedForeground;
using qs::windows::sameFocusReturnMonitor;

namespace {

QString readSource(const QString& name) {
	auto file = QFile(QDir(QStringLiteral(QS_WINDOWS_SRC)).filePath(name));
	if (!file.open(QFile::ReadOnly | QFile::Text)) return {};
	return QString::fromUtf8(file.readAll());
}

// The guard reasons about code, not the documentation that explains it. String
// and character literals are blanked too, so a token in text cannot satisfy a
// check and a `//` inside one cannot hide the rest of its line.
QString codeOnly(QString text) {
	auto inBlockComment = false;
	auto inLineComment = false;
	auto inString = false;
	auto inChar = false;
	auto escaped = false;

	for (auto index = 0; index < text.size(); index++) {
		auto character = text.at(index);

		if (inLineComment) {
			if (character == QLatin1Char('\n')) inLineComment = false;
			else text[index] = QLatin1Char(' ');
			continue;
		}

		if (inBlockComment) {
			if (character == QLatin1Char('*') && index + 1 < text.size()
			    && text.at(index + 1) == QLatin1Char('/'))
			{
				text[index] = QLatin1Char(' ');
				text[++index] = QLatin1Char(' ');
				inBlockComment = false;
			} else if (character != QLatin1Char('\n')) {
				text[index] = QLatin1Char(' ');
			}
			continue;
		}

		if (inString || inChar) {
			if (escaped) {
				escaped = false;
			} else if (character == QLatin1Char('\\')) {
				escaped = true;
			} else if (
			    (inString && character == QLatin1Char('"'))
			    || (inChar && character == QLatin1Char('\''))
			) {
				inString = false;
				inChar = false;
			}

			if (character != QLatin1Char('\n')) text[index] = QLatin1Char(' ');
			continue;
		}

		if (character == QLatin1Char('/') && index + 1 < text.size()) {
			auto next = text.at(index + 1);
			if (next == QLatin1Char('/') || next == QLatin1Char('*')) {
				text[index] = QLatin1Char(' ');
				text[++index] = QLatin1Char(' ');
				inLineComment = next == QLatin1Char('/');
				inBlockComment = next == QLatin1Char('*');
				continue;
			}
		}

		if (character == QLatin1Char('"')) {
			inString = true;
			text[index] = QLatin1Char(' ');
		} else if (character == QLatin1Char('\'')) {
			inChar = true;
			text[index] = QLatin1Char(' ');
		}
	}

	return text;
}

} // namespace

class ActivationPolicyTest: public QObject {
	Q_OBJECT;

private slots:
	void initTestCase() { QVERIFY(!currentActivationSource().has_value()); }

	void handoff_data() {
		QTest::addColumn<bool>("callerOwnsForeground");
		QTest::addColumn<uint>("targetPid");
		QTest::addColumn<uint>("callerPid");
		QTest::addColumn<bool>("allowTarget");

		QTest::newRow("foreground hands off to another process")
		    << true << 200u << 100u << true;
		QTest::newRow("foreground target is the caller")
		    << true << 100u << 100u << false;
		QTest::newRow("foreground without a target pid")
		    << true << 0u << 100u << false;
		QTest::newRow("another process owns the foreground")
		    << false << 200u << 100u << false;
		QTest::newRow("no foreground, own target")
		    << false << 100u << 100u << false;
	}

	void handoff() {
		QFETCH(bool, callerOwnsForeground);
		QFETCH(uint, targetPid);
		QFETCH(uint, callerPid);
		QFETCH(bool, allowTarget);

		auto result = foregroundHandoff(callerOwnsForeground, targetPid, callerPid);
		QCOMPARE(result == ForegroundHandoff::AllowTarget, allowTarget);
	}

	void sourceMayForce_data() {
		QTest::addColumn<int>("source");
		QTest::addColumn<bool>("mayForce");

		QTest::newRow("hook-swallowed key") << int(ActivationSource::Hook) << true;
		QTest::newRow("wm_hotkey") << int(ActivationSource::Hotkey) << false;
		QTest::newRow("shell click") << int(ActivationSource::Click) << false;
		QTest::newRow("ipc") << int(ActivationSource::Ipc) << false;
	}

	void sourceMayForce() {
		QFETCH(int, source);
		QFETCH(bool, mayForce);

		QCOMPARE(mayForceForeground(static_cast<ActivationSource>(source)), mayForce);
	}

	// A panel gives the foreground back to the application it took it from,
	// but only while the shell still owns it and the application is still
	// there. Otherwise the close path's next-window handoff runs.
	void rememberedForegroundRestore_data() {
		QTest::addColumn<bool>("shellOwnsForeground");
		QTest::addColumn<bool>("windowLive");
		QTest::addColumn<bool>("windowIsOwnProcess");
		QTest::addColumn<bool>("sameMonitor");
		QTest::addColumn<bool>("restore");

		QTest::newRow("own foreground, live same-monitor application") << true << true << false << true << true;
		QTest::newRow("own foreground, application on other monitor") << true << true << false << false << false;
		QTest::newRow("own foreground, window closed") << true << false << false << true << false;
		QTest::newRow("own foreground, window hidden") << true << false << false << true << false;
		QTest::newRow("own foreground, shell window") << true << true << true << true << false;
		QTest::newRow("another app owns the foreground") << false << true << false << true << false;
		QTest::newRow("no foreground and no window") << false << false << false << false << false;
	}

	void rememberedForegroundRestore() {
		QFETCH(bool, shellOwnsForeground);
		QFETCH(bool, windowLive);
		QFETCH(bool, windowIsOwnProcess);
		QFETCH(bool, sameMonitor);
		QFETCH(bool, restore);

		QCOMPARE(
		    shouldRestoreRememberedForeground(shellOwnsForeground, windowLive, windowIsOwnProcess, sameMonitor),
		    restore
		);
	}

	void focusReturnUsesExactNativeMonitor() {
		auto* panelMonitor = reinterpret_cast<HMONITOR>(1);
		auto* otherMonitor = reinterpret_cast<HMONITOR>(2);
		QVERIFY(sameFocusReturnMonitor(panelMonitor, panelMonitor));
		QVERIFY(!sameFocusReturnMonitor(panelMonitor, otherMonitor));
		QVERIFY(!sameFocusReturnMonitor(panelMonitor, nullptr));
		QVERIFY(!sameFocusReturnMonitor(nullptr, otherMonitor));
		QVERIFY(!sameFocusReturnMonitor(nullptr, nullptr));
	}

	// The tracker drops null-HWND foreground events, so a desktop or windowless
	// foreground reaches the remembered application only as the tracker losing
	// its active application.
	void rememberedForegroundClearsWhenTheTrackerLosesTheApplication() {
		RememberedForeground remembered;
		remembered.observe(reinterpret_cast<HWND>(1), ForegroundAction::Adopt);
		QCOMPARE(reinterpret_cast<quintptr>(remembered.hwnd), quintptr(1));

		// A shell overlay keeps the application through the shell's surfaces.
		remembered.observe(reinterpret_cast<HWND>(2), ForegroundAction::Keep);
		QCOMPARE(reinterpret_cast<quintptr>(remembered.hwnd), quintptr(1));

		// The desktop is already classified as Clear.
		remembered.observe(reinterpret_cast<HWND>(3), ForegroundAction::Clear);
		QVERIFY(remembered.hwnd == nullptr);

		remembered.observe(reinterpret_cast<HWND>(1), ForegroundAction::Adopt);
		remembered.onActiveApplicationLost();
		QVERIFY(remembered.hwnd == nullptr);
	}

	// The release has to be reachable from the focus change that unpins a
	// popout as well as from the close path, so both go through one helper.
	void panelReleasesTheForegroundOnFocusLoss() {
		auto panel = codeOnly(readSource(QStringLiteral("panel_window.cpp")));
		QVERIFY2(!panel.isEmpty(), "Could not read panel_window.cpp");

		QVERIFY2(
		    panel.contains(QStringLiteral("shouldRestoreRememberedForeground(")),
		    "panel_window.cpp does not gate the remembered foreground on the policy"
		);
		QVERIFY2(
		    panel.count(QStringLiteral("sameFocusReturnMonitor(")) >= 2,
		    "both remembered and fallback focus targets must be checked against the panel monitor"
		);
		QVERIFY2(
		    panel.contains(QStringLiteral("releaseForeground()")),
		    "panel_window.cpp has no shared foreground release"
		);
		QVERIFY2(
		    panel.contains(QStringLiteral("externalForeground()")),
		    "panel_window.cpp does not use the remembered external foreground"
		);
		QVERIFY2(
		    panel.contains(QStringLiteral("activeWindowChanged")),
		    "panel_window.cpp does not clear on the tracker losing its active application"
		);
	}

	void dispatchScopeCarriesAndRestoresSource() {		{
			ActivationDispatchScope hook(ActivationSource::Hook);
			QCOMPARE(
			    static_cast<int>(currentActivationSource().value_or(ActivationSource::Ipc)),
			    static_cast<int>(ActivationSource::Hook)
			);

			{
				ActivationDispatchScope hotkey(ActivationSource::Hotkey);
				QCOMPARE(
				    static_cast<int>(currentActivationSource().value_or(ActivationSource::Ipc)),
				    static_cast<int>(ActivationSource::Hotkey)
				);
			}

			QCOMPARE(
			    static_cast<int>(currentActivationSource().value_or(ActivationSource::Ipc)),
			    static_cast<int>(ActivationSource::Hook)
			);
		}

		QVERIFY(!currentActivationSource().has_value());
	}

	void deferredFirstShowKeepsHookSource() {
		PendingActivation pending;

		{
			ActivationDispatchScope hook(ActivationSource::Hook);
			pending.capture();
		}

		// Reloadable shows the window after the dispatch has ended.
		QVERIFY(!currentActivationSource().has_value());
		auto source = pending.take();
		QVERIFY(source.has_value());
		QCOMPARE(static_cast<int>(*source), static_cast<int>(ActivationSource::Hook));
	}

	void capturedSourceIsConsumedOnce() {
		PendingActivation pending;

		{
			ActivationDispatchScope hook(ActivationSource::Hook);
			pending.capture();
		}

		QVERIFY(pending.take().has_value());
		QVERIFY(!pending.take().has_value());

		// A later, unrelated dispatch does not inherit the consumed hook source.
		ActivationDispatchScope hotkey(ActivationSource::Hotkey);
		QVERIFY(!pending.take().has_value());
	}

	void unconsumedSourceDoesNotLeakAfterClear() {
		PendingActivation pending;

		{
			ActivationDispatchScope hook(ActivationSource::Hook);
			pending.capture();
		}

		pending.clear();
		QVERIFY(!pending.take().has_value());
	}

	void captureOutsideDispatchRecordsNothing() {
		PendingActivation pending;
		pending.capture();
		QVERIFY(!pending.take().has_value());

		// A request outside a dispatch does not erase an earlier captured one.
		{
			ActivationDispatchScope hook(ActivationSource::Hook);
			pending.capture();
		}

		pending.capture();
		QCOMPARE(
		    static_cast<int>(pending.take().value_or(ActivationSource::Ipc)),
		    static_cast<int>(ActivationSource::Hook)
		);
	}

	void deferredShowPathsCaptureAndConsumeTheSource() {
		struct Path {
			const char* file;
			const char* capture;
			const char* take;
			const char* clear;
		};

		static const auto paths = std::array {
		    Path {"../window/floatingwindow.cpp", "pendingOpen.capture()", "pendingOpen.take()", "pendingOpen.clear()"},
		    Path {"panel_window.cpp", "pendingOpen.capture()", "pendingOpen.take()", "pendingOpen.clear()"},
		    Path {"focus_grab.cpp", "pendingActivation.capture()", "pendingActivation.take()", "pendingActivation.clear()"},
		};

		for (const auto& path: paths) {
			auto source = codeOnly(readSource(QString::fromLatin1(path.file)))
			                  .remove(QRegularExpression(QStringLiteral(R"(\s+)")));
			QVERIFY2(!source.isEmpty(), path.file);

			// Exactly one capture where the open is requested, one consumption at the
			// show, and a clear so an open that never shows cannot leak.
			QVERIFY2(source.count(QString::fromLatin1(path.capture)) == 1, path.file);
			QVERIFY2(source.count(QString::fromLatin1(path.take)) == 1, path.file);
			QVERIFY2(source.contains(QString::fromLatin1(path.clear)), path.file);
		}

		// The floating window must not sample the source again at the deferred show.
		auto floating = codeOnly(readSource(QStringLiteral("../window/floatingwindow.cpp")));
		QCOMPARE(floating.count(QStringLiteral("activationSourceForCurrentState()")), 2);
	}

	void activationPathHasNoForcedEscalation() {
		static const auto files = QStringList {
		    QStringLiteral("focus_grab.cpp"),
		    QStringLiteral("panel_window.cpp"),
		    QStringLiteral("window_manager.cpp"),
		    QStringLiteral("window_tracker.cpp"),
		    QStringLiteral("../window/floatingwindow.cpp"),
		};
		static const auto forbidden = QStringList {
		    QStringLiteral("SendInput"),
		    QStringLiteral("AttachThreadInput"),
		    QStringLiteral("SetFocus("),
		    QStringLiteral("mouse_event"),
		    QStringLiteral("keybd_event"),
		    QStringLiteral("forceForegroundWindow"),
		    QStringLiteral("scheduleFocusGrab"),
		};

		for (const auto& name: files) {
			auto source = codeOnly(readSource(name));
			QVERIFY2(!source.isEmpty(), qPrintable(QStringLiteral("Could not read %1").arg(name)));

			for (const auto& token: forbidden) {
				QVERIFY2(
				    !source.contains(token),
				    qPrintable(QStringLiteral("%1 still contains %2").arg(name, token))
				);
			}
		}
	}

	void activationPathUsesOneRequest() {
		static const auto callers = QStringList {
		    QStringLiteral("focus_grab.cpp"),
		    QStringLiteral("panel_window.cpp"),
		    QStringLiteral("window_manager.cpp"),
		    QStringLiteral("window_tracker.cpp"),
		    QStringLiteral("../window/floatingwindow.cpp"),
		};

		for (const auto& name: callers) {
			auto source = codeOnly(readSource(name));
			QVERIFY2(!source.isEmpty(), qPrintable(QStringLiteral("Could not read %1").arg(name)));
			QVERIFY2(
			    source.contains(QStringLiteral("requestForegroundWindow(")),
			    qPrintable(QStringLiteral("%1 does not use requestForegroundWindow").arg(name))
			);
		}

		auto rawUtil = readSource(QStringLiteral("util.cpp"));
		QVERIFY2(!rawUtil.isEmpty(), "Could not read util.cpp");
		auto util = codeOnly(rawUtil);

		// The clean request is one call. The escalation below is the second and
		// only other direct call, and it is inside the hook-only helper.
		static const auto directCall =
		    QRegularExpression(QStringLiteral(R"((?<![A-Za-z_])SetForegroundWindow\()"));
		static const auto handoffCall =
		    QRegularExpression(QStringLiteral(R"((?<![A-Za-z_])AllowSetForegroundWindow\()"));
		QCOMPARE(util.count(directCall), 2);
		QCOMPARE(util.count(handoffCall), 1);

		// The accepted limitation lives in exactly one helper and is reachable
		// only through the source gate. No other activation file may attach to
		// the foreground input queue or set focus directly.
		QCOMPARE(util.count(QStringLiteral("AttachThreadInput(")), 2);
		QCOMPARE(util.count(QStringLiteral("SetFocus(")), 1);
		QCOMPARE(util.count(QStringLiteral("forceForegroundFromHook(")), 2);
		QVERIFY2(
		    util.contains(QStringLiteral("mayForceForeground(source) && forceForegroundFromHook(hwnd)")),
		    "util.cpp does not gate the hook escalation on the activation source"
		);

		// The limitation comment has to keep recording that the escalation is
		// accepted, so the next reader knows why it is still here.
		QVERIFY2(
		    rawUtil.contains(QStringLiteral("the accepted limitation is tracked in")),
		    "the hook escalation does not document its accepted limitation"
		);

		// A virtual-desktop switch is not an activation timer: the request is
		// part of the action that starts the switch.
		auto tracker = codeOnly(readSource(QStringLiteral("window_tracker.cpp")));
		QVERIFY2(
		    !tracker.contains(QStringLiteral("QTimer::singleShot")),
		    "window_tracker.cpp gates activation on a timer"
		);
	}
};

QTEST_GUILESS_MAIN(ActivationPolicyTest)

#include "activation_policy_test.moc"
