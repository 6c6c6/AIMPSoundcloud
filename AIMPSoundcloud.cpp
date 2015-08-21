#include "AIMPSoundcloud.h"

#include "AIMPString.h"
#include "SDK/apiPlayer.h"

HRESULT __declspec(dllexport) WINAPI AIMPPluginGetHeader(IAIMPPlugin **Header) {
    *Header = Plugin::instance();
    return S_OK;
}

Plugin *Plugin::m_instance = nullptr;

HRESULT WINAPI Plugin::Initialize(IAIMPCore *Core) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL) != Gdiplus::Status::Ok)
        return E_FAIL;

    m_core = Core;
    if (FAILED(Core->RegisterExtension(IID_IAIMPServiceOptionsDialog, new OptionsDialog(this))))
        return E_FAIL;

      if (!Config::Init(Core)) return E_FAIL;
    if (!AimpHTTP::Init(Core)) return E_FAIL;
    if (!AimpMenu::Init(Core)) return E_FAIL;

    Config::LoadExtendedConfig();

    m_accessToken = Config::GetString(L"AccessToken");

    if (AimpMenu *addMenu = AimpMenu::Get(AIMP_MENUID_PLAYER_PLAYLIST_ADDING)) {
        addMenu->Add(L"SoundCloud URL", AddURLDialog::Show, IDB_ICON)->Release();
        delete addMenu;
    }

    if (AimpMenu *playlistMenu = AimpMenu::Get(AIMP_MENUID_PLAYER_PLAYLIST_MANAGE)) {
        playlistMenu->Add(L"SoundCloud stream", SoundCloudAPI::LoadStream, IDB_ICON)->Release();
        playlistMenu->Add(L"SoundCloud likes", SoundCloudAPI::LoadLikes, IDB_ICON)->Release();
        delete playlistMenu;
    }

    if (AimpMenu *contextMenu = AimpMenu::Get(AIMP_MENUID_PLAYER_PLAYLIST_CONTEXT_FUNCTIONS)) {
        AimpMenu *recommendations = new AimpMenu(contextMenu->Add(L"Load recommendations", nullptr, IDB_ICON));
        recommendations->Add(L"Here", [this] {
            ForSelectedTracks([](IAIMPPlaylist *pl, IAIMPPlaylistItem *item, int64_t id) -> int {
                if (id > 0) {
                    SoundCloudAPI::LoadRecommendations(id, false);
                }
                return 0;
            });
        })->Release();
        recommendations->Add(L"Create new playlist", [this] {
            ForSelectedTracks([](IAIMPPlaylist *pl, IAIMPPlaylistItem *item, int64_t id) -> int {
                if (id > 0) {
                    SoundCloudAPI::LoadRecommendations(id, true);
                }
                return 0;
            });
        })->Release();
        delete recommendations;

        contextMenu->Add(L"Add to exclusions", [this] {
            ForSelectedTracks([](IAIMPPlaylist *pl, IAIMPPlaylistItem *item, int64_t id) -> int {
                if (id > 0) {
                    Config::TrackExclusions.insert(id);
                    return FLAG_DELETE_ITEM;
                }
                return 0;
            });
            Config::SaveExtendedConfig();
        }, IDB_ICON)->Release();

        contextMenu->Add(L"Unlike", [this] {
            ForSelectedTracks([](IAIMPPlaylist *pl, IAIMPPlaylistItem *item, int64_t id) -> int {
                if (id > 0) {
                    SoundCloudAPI::UnlikeSong(id);
                }
                return 0;
            });
        }, IDB_ICON)->Release();

        contextMenu->Add(L"Like", [this] {
            ForSelectedTracks([](IAIMPPlaylist *pl, IAIMPPlaylistItem *item, int64_t id) -> int {
                if (id > 0) {
                    SoundCloudAPI::LikeSong(id);
                }
                return 0;
            });
            // TODO: should wait for result
            SoundCloudAPI::LoadLikes();
        }, IDB_ICON)->Release();
        delete contextMenu;
    }

    if (FAILED(m_core->QueryInterface(IID_IAIMPServicePlaylistManager, reinterpret_cast<void **>(&m_playlistManager))))
        return E_FAIL;

    if (FAILED(m_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, reinterpret_cast<void **>(&m_messageDispatcher))))
        return E_FAIL;

    m_messageHook = new MessageHook(this);
    if (FAILED(m_messageDispatcher->Hook(m_messageHook))) {
        delete m_messageHook;
        return E_FAIL;
    }

    return S_OK;
}

HRESULT WINAPI Plugin::Finalize() {
    m_messageDispatcher->Unhook(m_messageHook);
    m_messageDispatcher->Release();
    m_playlistManager->Release();

    AimpMenu::Deinit();
    AimpHTTP::Deinit();
      Config::Deinit();

    Gdiplus::GdiplusShutdown(m_gdiplusToken);
    return S_OK;
}

IAIMPPlaylist *Plugin::GetPlaylist(const std::wstring &playlistName, bool activate) {
    IAIMPPlaylist *playlistPointer = nullptr;
    if (SUCCEEDED(m_playlistManager->GetLoadedPlaylistByName(new AIMPString(playlistName), &playlistPointer)) && playlistPointer) {
        if (activate)
            m_playlistManager->SetActivePlaylist(playlistPointer);

        return UpdatePlaylistGrouping(playlistPointer);
    } else {
        if (SUCCEEDED(m_playlistManager->CreatePlaylist(new AIMPString(playlistName), activate, &playlistPointer)))
            return UpdatePlaylistGrouping(playlistPointer);
    }

    return nullptr;
}

IAIMPPlaylist *Plugin::GetCurrentPlaylist() {
    IAIMPPlaylist *pl = nullptr;
    if (SUCCEEDED(m_playlistManager->GetActivePlaylist(&pl))) {
        return pl;
    }
    return nullptr;
}

IAIMPPlaylist *Plugin::UpdatePlaylistGrouping(IAIMPPlaylist *pl) {
    if (!pl)
        return nullptr;

    // Change playlist grouping template to %A (album)
    IAIMPPropertyList *plProp = nullptr;
    if (SUCCEEDED(pl->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void **>(&plProp)))) {
        plProp->SetValueAsInt32(AIMP_PLAYLIST_PROPID_GROUPPING_OVERRIDEN, 1);
        plProp->SetValueAsObject(AIMP_PLAYLIST_PROPID_GROUPPING_TEMPLATE, new AIMPString(L"%A"));
        plProp->Release();
    }
    return pl;
}

IAIMPPlaylistItem *Plugin::GetCurrentTrack() {
    IAIMPServicePlayer *player = nullptr;
    if (SUCCEEDED(m_core->QueryInterface(IID_IAIMPServicePlayer, reinterpret_cast<void **>(&player)))) {
        IAIMPPlaylistItem *item = nullptr;
        if (SUCCEEDED(player->GetPlaylistItem(&item))) {
            player->Release();
            return item;
        }
        player->Release();
    }
    return nullptr;
}

void Plugin::ForSelectedTracks(std::function<int(IAIMPPlaylist *, IAIMPPlaylistItem *, int64_t)> callback) {
    if (!callback)
        return;

    if (IAIMPPlaylist *pl = GetCurrentPlaylist()) {
        pl->BeginUpdate();
        std::set<IAIMPPlaylistItem *> to_del;
        auto delPending = [&] {
            for (auto x : to_del) {
                pl->Delete(x);
                x->Release();
            }
        };
        for (int i = 0, n = pl->GetItemCount(); i < n; ++i) {
            IAIMPPlaylistItem *item = nullptr;
            if (SUCCEEDED(pl->GetItem(i, IID_IAIMPPlaylistItem, reinterpret_cast<void **>(&item)))) {
                int isSelected = 0;
                if (SUCCEEDED(item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, &isSelected))) {
                    if (isSelected) {
                        IAIMPString *url = nullptr;
                        if (SUCCEEDED(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, reinterpret_cast<void **>(&url)))) {
                            int64_t id = Tools::TrackIdFromUrl(url->GetData());
                            url->Release();
                            
                            int result = callback(pl, item, id);
                            if (result & FLAG_DELETE_ITEM) {
                                to_del.insert(item);
                                if (result & FLAG_STOP_LOOP) {
                                    delPending();
                                    pl->EndUpdate();
                                    pl->Release();
                                    return;
                                }
                                continue;
                            }
                            if (result & FLAG_STOP_LOOP) {
                                delPending();
                                item->Release();
                                pl->EndUpdate();
                                pl->Release();
                                return;
                            }
                        }
                    }
                }
                item->Release();
            }
        }
        delPending();
        pl->EndUpdate();
        pl->Release();
    }
}

void Plugin::ForEveryItem(IAIMPPlaylist *pl, std::function<int(IAIMPPlaylistItem *, IAIMPFileInfo *, int64_t)> callback) {
    if (!pl || !callback)
        return;

    pl->BeginUpdate();
    for (int i = 0, n = pl->GetItemCount(); i < n; ++i) {
        IAIMPPlaylistItem *item = nullptr;
        if (SUCCEEDED(pl->GetItem(i, IID_IAIMPPlaylistItem, reinterpret_cast<void **>(&item)))) {
            IAIMPFileInfo *finfo = nullptr;
            if (SUCCEEDED(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO, IID_IAIMPFileInfo, reinterpret_cast<void **>(&finfo)))) {
                IAIMPString *custom = nullptr;
                if (SUCCEEDED(finfo->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, reinterpret_cast<void **>(&custom)))) {
                    std::wstring url(custom->GetData());
                    custom->Release();

                    int64_t id = Tools::TrackIdFromUrl(url);
                    int result = callback(item, finfo, id);
                    if (result & FLAG_DELETE_ITEM) {
                        pl->Delete(item);
                        finfo->Release();
                        item->Release();
                        i--;
                        n--;
                        if (result & FLAG_STOP_LOOP) {
                            pl->EndUpdate();
                            return;
                        }
                        continue;
                    }
                    if (result & FLAG_STOP_LOOP) {
                        finfo->Release();
                        item->Release();
                        pl->EndUpdate();
                        return;
                    }
                }
                finfo->Release();
            }
            item->Release();
        }
    }
    pl->EndUpdate();
}

HWND Plugin::GetMainWindowHandle() {
    HWND handle = NULL;
    if (SUCCEEDED(m_messageDispatcher->Send(AIMP_MSG_PROPERTY_HWND, AIMP_MSG_PROPVALUE_GET, &handle))) {
        return handle;
    }
    return NULL;
}
