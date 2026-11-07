# Wi-Fi identity and security component fixture

## Consumer contract

`NetworkWifiNetwork.ssid` remains display text. It is not a lossless identifier.
`rawSsidHex` preserves the length-delimited bytes. `selectionId` includes those
bytes, selected adapter GUID, authentication, cipher, BSS type, security flag
and exact saved profile name. It contains network information, not anonymized
diagnostic data. Do not log it by default.

New consumers should retain `selectionId` and call `connectToNetworkById(id,
password)` or `forgetNetworkById(id)`. Removed IDs are refused. The legacy
display-name methods work only for a unique current row, never fabricate an
unknown open network, and refuse ambiguous names. A display-only caller cannot
express historical identity after a label is reused. Coordinated shell adoption
of the ID methods is needed for the full stale-selection guarantee.

Rows expose numeric `authentication`, numeric `cipher`, `newProfileSupported`
and `newProfileReason` independently of the compatibility `security` label.
New XML supports only open/none, WPA2-PSK/CCMP and WPA3-SAE/CCMP infrastructure
networks. Everything else is refused before `WlanSetProfile`. Supported personal
XML takes an ASCII passphrase of 8 through 63 characters. This is not a new
credential UI, credential provisioning or enterprise enrollment.

Saved profiles bypass the new-profile restriction and use the exact Windows
profile name, including whitespace and Unicode. Missing names are refused, not
replaced with display text. Generated profile names are ASCII derived from raw
bytes; SSID XML uses only `<hex>`. Existing profiles are never overwritten.
If profile creation succeeds but connection submission fails, the created
profile can remain. No destructive rollback is attempted.

The existing shell's unsaved-secure-network Windows Settings handoff is
unchanged. WEP with open authentication still has a nonempty security label.
No shell file, gitlink, source lock or build registration changes are part of
this backend patch.

## Request results

`lastWifiCommand` and `wifiCommandResult(result)` carry `requestId` (decimal
string), `selectionId`, `status` and `reason`. An immediate refusal is `denied`
with an empty request ID. `accepted` means `WlanConnect` accepted the request,
not that a connection completed. Only a matching interface/raw SSID/exact
profile completion notification produces `observed`; failure produces `failed`.
The legacy `wifiConnectResult` remains terminal-only.

Concurrent requests are refused. Cancellation ends tracking as `cancelled`;
it does not disconnect the adapter's potentially unrelated current connection
or claim that Windows cancelled the submitted attempt. The separate explicit
Disconnect action requests that operation, without a completion claim. Removing
the adapter ends a pending request as a failure. Queued notifications from an
earlier request cannot complete a later request; unrelated notifications and
display-string connection observations cannot end the pending request.
An accepted request has a 60-second completion-observation deadline. On expiry,
tracking ends as `failed/completion-unobserved`, leaving Windows' connection
untouched and allowing another explicit request. This is not a claim that the
OS connection failed. Matching completion, cancellation and adapter removal
stop the timer. Display-only observations still cannot claim completion.

Available-network enumeration drops a profileless twin only when a saved row
matches its adapter, raw SSID bytes, authentication, cipher, BSS type and security
flag. Distinct saved profiles and security variants remain separate choices.

Windows supplies no application request ID in these notifications. A matching
observation is evidence of the reported connection, not proof of causality or
credential enrollment. An indistinguishable same-target event delivered only
after another identical request starts cannot be attributed more precisely.

## Run without live WLAN

From PowerShell with installed Visual Studio C++ tools, Qt 6.11.2 and LLVM:

```powershell
.\src\windows\test\Invoke-WifiIdentityFixture.ps1 `
  -QtPrefix <Qt>\6.11.2\msvc2022_64 `
  -LlvmBin <LLVM>\bin `
  -OutputDirectory <dedicated-output>\native -BuildJobs 2
```

The script builds the actual `network.cpp`, `network_wifi.cpp`,
`network_wifi_item.cpp` and object model. It deliberately does not link
`wlanapi.lib`. Every WLAN entry point resolves to a synthetic fake.
Unexpected open, scan, radio mutation or asynchronous startup is fatal.
Connectivity observation is stubbed. Outputs are text and JUnit XML receipts.
The runner rejects compilation/link failures, test failures and empty suites.
Tool directories are read-only inputs.

Coverage includes invalid UTF-8, embedded NUL, all-NUL and 32-byte SSIDs;
invalid lengths; colliding displays; security/cipher combinations; saved
profiles including unsupported new modes; model/property snapshot consumers;
BSS byte identity; exact profile matching; stale IDs/adapter replacement;
accepted versus terminal results; cancellation and reentrancy.

This is native component evidence with injected Windows outcomes, not WLAN
hardware acceptance. Controlled real-network connections, OS acceptance of
generated XML, downstream exact-pair shell tests and recovery/permission
scenarios remain separate qualification work.

Regression controls should fail for substantive reasons when independently
changing SSID XML hex back to `network.ssid.toUtf8()`, accepting every secured
mode, comparing notification SSIDs as display text, or replacing the saved
profile name with display text. Restore each change before normal validation.
The fixture checks the bytes passed through XML, fake profile-write counts,
terminal result counts and exact profile names respectively.

## Registration handoff

The shared test CMake files are intentionally untouched. A build-file owner can
register `network_wifi_identity_test` with:

* `network_wifi_identity_test.cpp`, `network_wifi_native_stubs.cpp` and
  `network_wifi_stubs.cpp`;
* production `system/network.cpp/.hpp`, `system/network_wifi.cpp/.hpp`,
  `system/network_wifi_item.cpp/.hpp`, `system/network_connectivity.hpp` and
  `../core/model.cpp`;
* `QS_TEST`, `QS_WIFI_NATIVE_FIXTURE`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`;
* Qt Core, Qml, Test and shell32, without wlanapi or the real connectivity,
  startup, background-pool or application logging implementations.

The existing `network_wifi_route_test` target keeps its ordinary link stubs.
Its routing and completed-snapshot tests now use a single Qt Test suite so
result files retain every case instead of overwriting the first suite.
