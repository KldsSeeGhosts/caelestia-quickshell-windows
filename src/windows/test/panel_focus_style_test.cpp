#include <qcoreapplication.h>
#include <qtest.h>

#include "../panel_focus_style.hpp"

class PanelFocusStyleTest: public QObject {
	Q_OBJECT;

private slots:
	void noneToOnDemandToNoneIsImmediate() {
		using namespace qs::windows;
		using namespace qs::windows::panel_focus_style;

		auto style = applyExtendedStyle(0, PanelKeyboardFocus::None);
		QVERIFY((style & toolWindow) != 0);
		QVERIFY((style & noActivate) != 0);

		style = applyExtendedStyle(style, PanelKeyboardFocus::OnDemand);
		QVERIFY((style & toolWindow) != 0);
		QVERIFY((style & noActivate) == 0);

		style = applyExtendedStyle(style, PanelKeyboardFocus::None);
		QVERIFY((style & toolWindow) != 0);
		QVERIFY((style & noActivate) != 0);
	}

	void exclusiveAllowsActivation() {
		using namespace qs::windows;
		using namespace qs::windows::panel_focus_style;

		auto style = applyExtendedStyle(noActivate, PanelKeyboardFocus::Exclusive);
		QVERIFY((style & toolWindow) != 0);
		QVERIFY((style & noActivate) == 0);
	}

	void surfaceRecreationReappliesStyles() {
		using namespace qs::windows;
		using namespace qs::windows::panel_focus_style;

		// A replacement native surface starts without styles derived from the
		// old HWND. Reapplying the policy must restore both focused and
		// non-focusable states.
		auto recreatedFocused = applyExtendedStyle(0, PanelKeyboardFocus::OnDemand);
		QVERIFY((recreatedFocused & toolWindow) != 0);
		QVERIFY((recreatedFocused & noActivate) == 0);

		auto recreatedNonFocusable = applyExtendedStyle(0, PanelKeyboardFocus::None);
		QVERIFY((recreatedNonFocusable & toolWindow) != 0);
		QVERIFY((recreatedNonFocusable & noActivate) != 0);
	}

	void unrelatedExtendedStylesArePreserved() {
		using namespace qs::windows;
		using namespace qs::windows::panel_focus_style;

		constexpr std::uint32_t unrelated = 0x00080000u | 0x00040000u | 0x00000008u;

		auto focusable = applyExtendedStyle(unrelated, PanelKeyboardFocus::OnDemand);
		QCOMPARE(focusable & unrelated, unrelated);
		QVERIFY((focusable & toolWindow) != 0);
		QVERIFY((focusable & noActivate) == 0);

		auto nonFocusable = applyExtendedStyle(focusable, PanelKeyboardFocus::None);
		QCOMPARE(nonFocusable & unrelated, unrelated);
		QVERIFY((nonFocusable & toolWindow) != 0);
		QVERIFY((nonFocusable & noActivate) != 0);
	}

	void noneRejectsKeyboardFocusGrab() {
		using namespace qs::windows;
		using namespace qs::windows::panel_focus_style;

		QVERIFY(!allowsKeyboardFocusGrab(PanelKeyboardFocus::None));
		QVERIFY(allowsKeyboardFocusGrab(PanelKeyboardFocus::OnDemand));
		QVERIFY(allowsKeyboardFocusGrab(PanelKeyboardFocus::Exclusive));
	}

	void mouseActivationFollowsFocusPolicy() {
		using namespace qs::windows;
		using namespace qs::windows::panel_focus_style;

		auto none = mouseActivationResult(PanelKeyboardFocus::None);
		QVERIFY(none.has_value());
		QCOMPARE(*none, noActivateMouseResult);
		QVERIFY(!mouseActivationResult(PanelKeyboardFocus::OnDemand).has_value());
		QVERIFY(!mouseActivationResult(PanelKeyboardFocus::Exclusive).has_value());
	}
};

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	PanelFocusStyleTest test;
	return QTest::qExec(&test, argc, argv);
}

#include "panel_focus_style_test.moc"
