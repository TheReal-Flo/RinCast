#include "firewall.h"

#include <netfw.h>
#include <netlistmgr.h>
#include <windows.h>
#include <wrl/client.h>

#include <map>
#include <vector>

#include "../util.h"
#include "rc/socket.h"

using Microsoft::WRL::ComPtr;

namespace rcs::firewall {

namespace {

constexpr wchar_t kUsbRule[] = L"RinCast Streamer (USB cable)";

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The category of the network each connected adapter is on, by adapter GUID ("{...}", lower case).
std::map<std::string, NLM_NETWORK_CATEGORY> NetworkCategories() {
    std::map<std::string, NLM_NETWORK_CATEGORY> out;
    ComPtr<INetworkListManager> nlm;
    ComPtr<IEnumNetworkConnections> connections;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm))) ||
        FAILED(nlm->GetNetworkConnections(&connections)))
        return out;
    ComPtr<INetworkConnection> c;
    while (connections->Next(1, &c, nullptr) == S_OK) {
        GUID adapter{};
        ComPtr<INetwork> network;
        NLM_NETWORK_CATEGORY category = NLM_NETWORK_CATEGORY_PUBLIC;
        if (SUCCEEDED(c->GetAdapterId(&adapter)) && SUCCEEDED(c->GetNetwork(&network)) &&
            SUCCEEDED(network->GetCategory(&category))) {
            wchar_t id[64];
            StringFromGUID2(adapter, id, 64);
            out[Lower(Narrow(id))] = category;
        }
        c.Reset();
    }
    return out;
}

// The interfaces a rule is limited to, or none for a rule that applies to all of them.
std::vector<std::string> RuleInterfaces(INetFwRule* rule) {
    std::vector<std::string> out;
    VARIANT v;
    VariantInit(&v);
    if (FAILED(rule->get_Interfaces(&v))) return out;
    if ((v.vt & VT_ARRAY) && v.parray) {
        VARIANT* items = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(v.parray, reinterpret_cast<void**>(&items)))) {
            for (ULONG i = 0; i < v.parray->rgsabound[0].cElements; i++)
                if (items[i].vt == VT_BSTR) out.push_back(Narrow(items[i].bstrVal));
            SafeArrayUnaccessData(v.parray);
        }
    }
    VariantClear(&v);
    return out;
}

std::wstring Base64(const std::wstring& text) {
    // PowerShell's -EncodedCommand: UTF-16LE in base64, so paths with quotes or spaces need no escaping.
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size() * 2;
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = bytes[i] << 16 | (i + 1 < n ? bytes[i + 1] << 8 : 0) | (i + 2 < n ? bytes[i + 2] : 0);
        out += wchar_t(k[(v >> 18) & 63]);
        out += wchar_t(k[(v >> 12) & 63]);
        out += i + 1 < n ? wchar_t(k[(v >> 6) & 63]) : L'=';
        out += i + 2 < n ? wchar_t(k[v & 63]) : L'=';
    }
    return out;
}

std::wstring Quote(const std::wstring& s) {
    std::wstring out = L"'";
    for (wchar_t c : s) out += c == L'\'' ? std::wstring(L"''") : std::wstring(1, c);
    return out + L"'";
}

// One elevated PowerShell script; exit code 0 when it went through.
bool RunScript(const std::wstring& script) {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring ps = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::wstring wrapped = L"$ErrorActionPreference='Stop'; try { " + script + L" } catch { exit 1 }";
    return RunElevated(ps, L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + Base64(wrapped), true) == 0;
}

// Keeps the USB cable rule's adapters through a rewrite of the program's rules.
const wchar_t kKeepUsb[] =
    L"$usb = @(Get-NetFirewallRule -DisplayName 'RinCast Streamer (USB cable)' -ErrorAction SilentlyContinue | "
    L"Get-NetFirewallInterfaceFilter | ForEach-Object InterfaceAlias | Where-Object { $_ -and $_ -ne 'Any' }); ";

std::wstring AddUsbRule(const std::wstring& exe) {
    return L"Remove-NetFirewallRule -DisplayName '" + std::wstring(kUsbRule) + L"' -ErrorAction SilentlyContinue; "
           L"if ($usb.Count) { New-NetFirewallRule -DisplayName '" + std::wstring(kUsbRule) +
           L"' -Direction Inbound -Action Allow -Protocol UDP -Profile Any -Program " + Quote(exe) +
           L" -InterfaceAlias ($usb | Select-Object -Unique) | Out-Null }";
}

}  // namespace

Status Query(const std::wstring& exe) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Status status;  // when Windows can't tell, don't raise a false alarm
    {
        // Networks by category, the USB cable's apart.
        const auto categories = NetworkCategories();
        NLM_NETWORK_CATEGORY usbCategory = NLM_NETWORK_CATEGORY_PUBLIC;
        bool publicElsewhere = false, haveCategories = !categories.empty();
        std::vector<std::string> usbIds;
        for (const auto& itf : rc::ListInterfacesV6())
            if (itf.usbNcm) {
                if (status.usbAdapter.empty()) status.usbAdapter = itf.name;
                usbIds.push_back(Lower(itf.adapterId));
            }
        for (const auto& [id, category] : categories) {
            bool usb = false;
            for (const auto& u : usbIds) usb = usb || u == id;
            if (usb) usbCategory = category;
            else if (category == NLM_NETWORK_CATEGORY_PUBLIC) publicElsewhere = true;
        }

        ComPtr<INetFwPolicy2> policy;
        ComPtr<INetFwRules> rules;
        ComPtr<IUnknown> enumUnknown;
        ComPtr<IEnumVARIANT> rulesEnum;
        long active = 0;
        if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy))) &&
            SUCCEEDED(policy->get_CurrentProfileTypes(&active)) && SUCCEEDED(policy->get_Rules(&rules)) &&
            SUCCEEDED(rules->get__NewEnum(&enumUnknown)) && SUCCEEDED(enumUnknown.As(&rulesEnum))) {
            // Profiles that stop an inbound packet unless a rule allows it.
            long guarded = 0;
            for (const NET_FW_PROFILE_TYPE2 p : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
                VARIANT_BOOL enabled = VARIANT_TRUE;
                NET_FW_ACTION fallback = NET_FW_ACTION_BLOCK;
                policy->get_FirewallEnabled(p, &enabled);
                policy->get_DefaultInboundAction(p, &fallback);
                if (enabled != VARIANT_FALSE && fallback == NET_FW_ACTION_BLOCK) guarded |= p;
            }
            long allowed = 0, blocked = 0;
            bool usbRule = false;
            VARIANT item;
            VariantInit(&item);
            while (rulesEnum->Next(1, &item, nullptr) == S_OK) {
                ComPtr<INetFwRule> rule;
                if (item.vt == VT_DISPATCH && item.pdispVal && SUCCEEDED(item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) {
                    VARIANT_BOOL enabled = VARIANT_FALSE;
                    NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
                    BSTR program = nullptr;
                    rule->get_Enabled(&enabled);
                    rule->get_Direction(&direction);
                    if (enabled != VARIANT_FALSE && direction == NET_FW_RULE_DIR_IN && SUCCEEDED(rule->get_ApplicationName(&program)) && program &&
                        _wcsicmp(program, exe.c_str()) == 0) {
                        long protocol = 0, profiles = 0;
                        NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
                        rule->get_Protocol(&protocol);
                        rule->get_Profiles(&profiles);
                        rule->get_Action(&action);
                        if (protocol == NET_FW_IP_PROTOCOL_UDP || protocol == NET_FW_IP_PROTOCOL_ANY) {
                            const auto interfaces = RuleInterfaces(rule.Get());
                            if (interfaces.empty()) {
                                (action == NET_FW_ACTION_ALLOW ? allowed : blocked) |= profiles;
                            } else if (action == NET_FW_ACTION_ALLOW && !status.usbAdapter.empty()) {
                                // Limited to some adapters: only says something about those.
                                for (const auto& i : interfaces) usbRule = usbRule || Lower(i) == Lower(status.usbAdapter);
                            }
                        }
                    }
                    if (program) SysFreeString(program);
                }
                VariantClear(&item);
            }
            // A block rule wins over an allow rule.
            const long reachable = allowed & ~blocked;
            const long otherInUse = active & guarded & ~long(NET_FW_PROFILE2_PUBLIC);
            status.otherNetworksOpen = (otherInUse & reachable) == otherInUse;
            status.publicInUse = haveCategories ? publicElsewhere : (active & NET_FW_PROFILE2_PUBLIC) != 0;
            status.publicUnguarded = (guarded & NET_FW_PROFILE2_PUBLIC) == 0;
            status.publicAllowed = status.publicUnguarded || (reachable & NET_FW_PROFILE2_PUBLIC) != 0;
            if (!status.usbAdapter.empty()) {
                const long usbProfile = usbCategory == NLM_NETWORK_CATEGORY_PRIVATE ? NET_FW_PROFILE2_PRIVATE
                                        : usbCategory == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED ? NET_FW_PROFILE2_DOMAIN
                                                                                                   : NET_FW_PROFILE2_PUBLIC;
                status.usbOpen = usbRule || (guarded & usbProfile) == 0 || (reachable & usbProfile) != 0;
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return status;
}

bool Set(const std::wstring& exe, bool allowPublic) {
    // Dismissing Windows' own prompt leaves block rules for the program, so those go first.
    // Scoped to this program, not to ports: nothing else gets in on a public network.
    const std::wstring script =
        std::wstring(kKeepUsb) + L"$exe = " + Quote(exe) + L"; "
        L"Get-NetFirewallApplicationFilter -Program $exe -ErrorAction SilentlyContinue | Get-NetFirewallRule | "
        L"Where-Object Direction -eq Inbound | Remove-NetFirewallRule; "
        L"New-NetFirewallRule -DisplayName 'RinCast Streamer' -Direction Inbound -Action Allow -Protocol UDP -Program $exe "
        L"-Profile " + std::wstring(allowPublic ? L"Domain,Private,Public" : L"Domain,Private") + L" | Out-Null; " + AddUsbRule(exe);
    return RunScript(script);
}

bool AllowUsbAdapter(const std::wstring& exe, const std::string& adapter) {
    if (adapter.empty()) return false;
    wchar_t name[256];
    MultiByteToWideChar(CP_UTF8, 0, adapter.c_str(), -1, name, 256);
    return RunScript(std::wstring(kKeepUsb) + L"$usb += " + Quote(name) + L"; " + AddUsbRule(exe));
}

}  // namespace rcs::firewall
