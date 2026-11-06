#pragma once

#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.h>

#include "backend_types.hpp"

namespace qs::bluetooth {

struct RadioResult {
	winrt::Windows::Foundation::AsyncStatus status;
	winrt::Windows::Devices::Radios::RadioAccessStatus access =
	    winrt::Windows::Devices::Radios::RadioAccessStatus::Unspecified;
	winrt::hresult error {};
};

// Read both operations the same way, including an ErrorCode supplied by an
// asynchronous failure and a GetResults exception after Completed.
template <typename Operation>
RadioResult readRadioResult(const Operation& op, winrt::Windows::Foundation::AsyncStatus status) {
	RadioResult result {status};
	try {
		if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
			result.access = op.GetResults();
		} else if (status == winrt::Windows::Foundation::AsyncStatus::Error) {
			result.error = op.ErrorCode();
		}
	} catch (const winrt::hresult_error& e) {
		result.status = winrt::Windows::Foundation::AsyncStatus::Error;
		result.error = e.code();
	}
	return result;
}

inline void projectRadioResult(AdapterSnapshot& snapshot, const RadioResult& result) {
	using namespace winrt::Windows::Devices::Radios;
	using winrt::Windows::Foundation::AsyncStatus;
	snapshot.radioAccessDenied = result.error == winrt::hresult(static_cast<qint32>(0x80070005u))
	                          || (result.status == AsyncStatus::Completed
	                              && (result.access == RadioAccessStatus::DeniedByUser
	                                  || result.access == RadioAccessStatus::DeniedBySystem));
	if (result.error.value != 0) {
		snapshot.lastError = QStringLiteral("0x")
		                   + QString::number(static_cast<quint32>(result.error.value), 16)
		                         .rightJustified(8, QLatin1Char('0'))
		                         .toUpper();
	} else if (result.status != AsyncStatus::Completed) {
		snapshot.lastError = result.status == AsyncStatus::Canceled
		                       ? QStringLiteral("Canceled")
		                       : QStringLiteral("RadioOperationFailed");
	} else {
		switch (result.access) {
		case RadioAccessStatus::Allowed: snapshot.lastError.clear(); break;
		case RadioAccessStatus::DeniedByUser:
			snapshot.lastError = QStringLiteral("DeniedByUser");
			break;
		case RadioAccessStatus::DeniedBySystem:
			snapshot.lastError = QStringLiteral("DeniedBySystem");
			break;
		default: snapshot.lastError = QStringLiteral("Unspecified"); break;
		}
	}
}

} // namespace qs::bluetooth
