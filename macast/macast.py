# Copyright (c) 2021 by xfangfang. All Rights Reserved.

import os
import re
import sys
import time
import json
import atexit
import shlex
import cherrypy
import logging
import threading
import requests
import pyperclip
import gettext
import importlib

from .utils import SettingProperty, SETTING_DIR, notify_error, format_class_name
from .gui import App, MenuItem, Platform
from .protocol import DLNAProtocol
from .server import Service
from .utils import RENDERER_DIR, PROTOCOL_DIR, Setting
from .airplay import AirPlayProcessManager
from .bluetooth_audio import BluetoothAudioProcessManager
from macast_renderer.mpv import MPVRenderer

logger = logging.getLogger("main")
logger.setLevel(logging.DEBUG)
_ = gettext.gettext


class MacastPlugin:

    def __init__(self, path, title="None", plugin_instance=None, platform='none'):
        # path is allowed to be set to None only when renderer is macast default plugin
        self.path = path
        self.title = title
        self.plugin_class = None
        self.plugin_instance = plugin_instance
        self.platform = platform
        if path:
            try:
                self.load_from_file(path)
            except Exception as e:
                cherrypy.engine.publish('app_notify', 'ERROR', 'Custom plugin load error.')
                logger.error(str(e))

    def get_info(self):
        props = ['protocol', 'title', 'renderer', 'platform', 'version', 'author', 'desc']
        res = {'default': False}
        for i in props:
            res[i] = getattr(self, i, '')
        if getattr(self, 'renderer', None) is not None:
            res['type'] = 'renderer'
        if getattr(self, 'protocol', None) is not None:
            res['type'] = 'protocol'
        if self.path is None:
            res['default'] = True
            res['desc'] = 'Macast default plugin'
            res['version'] = Setting.version
        return res

    def get_instance(self):
        if self.plugin_instance is None and self.plugin_class is not None:
            self.plugin_instance = self.plugin_class()

        return self.plugin_instance

    def check(self):
        """ Check if this renderer can run on your device
        """
        if self.plugin_class is None:
            return False
        if sys.platform in self.platform:
            return True

        logger.error("{} support platform: {}".format(self.title, self.platform))
        logger.error("{} is not suit for this system.".format(self.title))
        return False

    def load_from_file(self, path):
        base_name = os.path.basename(path)[:-3]
        with open(path, 'r', encoding='utf-8') as f:
            renderer_file = f.read()
            metadata = re.findall("<macast.(.*?)>(.*?)</macast", renderer_file)
            print("<Load Plugin from {}".format(base_name))
            for key, value in metadata:
                print('%-10s: %s' % (key, value))
                setattr(self, key, str(value))
        if hasattr(self, 'renderer'):
            module = importlib.import_module(f'{RENDERER_DIR}.{base_name}')
            print(f'Load plugin {self.renderer} done />\n')
            self.plugin_class = getattr(module, self.renderer, None)
        elif hasattr(self, 'protocol'):
            module = importlib.import_module(f'{PROTOCOL_DIR}.{base_name}')
            print(f'Load plugin {self.protocol} done />\n')
            self.plugin_class = getattr(module, self.protocol, None)
        else:
            logger.error(f"Cannot find any plugin in {base_name}")
            return


class MacastPluginManager:

    def __init__(self, renderer_default, protocol_default):
        sys.path.append(SETTING_DIR)
        self.create_plugin_dir(RENDERER_DIR)
        self.create_plugin_dir(PROTOCOL_DIR)
        self.renderer_list = [renderer_default]
        self.renderer_list += self.load_macast_plugin(RENDERER_DIR)
        self.protocol_list = [protocol_default]
        self.protocol_list += self.load_macast_plugin(PROTOCOL_DIR)

    def get_renderer(self, name):
        plugin = self.get_plugin_from_list(self.renderer_list, name)
        Setting.set(SettingProperty.Macast_Renderer, plugin.title)
        return plugin.get_instance()

    def get_protocol(self, name):
        plugin = self.get_plugin_from_list(self.protocol_list, name)
        Setting.set(SettingProperty.Macast_Protocol, plugin.title)
        return plugin.get_instance()

    def get_info(self):
        res = []
        for r in self.renderer_list:
            res.append(r.get_info())
        for p in self.protocol_list:
            res.append(p.get_info())
        return res

    @staticmethod
    def get_plugin_from_list(plugin_list, title) -> MacastPlugin:
        for i in plugin_list:
            if title == i.title:
                print("using plugin: {}".format(title))
                return i
        else:
            print("using default plugin")
            return plugin_list[0]

    @staticmethod
    def load_macast_plugin(path: str):
        plugin_path = os.path.join(SETTING_DIR, path)
        if not os.path.exists(plugin_path):
            return
        plugin_list = []
        plugins = os.listdir(plugin_path)
        plugins = filter(lambda s: s.endswith('.py') and s != '__init__.py', plugins)
        for plugin in plugins:
            path = os.path.join(plugin_path, plugin)
            plugin_config = MacastPlugin(path)
            if plugin_config.check():
                plugin_list.append(plugin_config)
        return plugin_list

    @staticmethod
    @notify_error('Cannot create custom plugin dir.')
    def create_plugin_dir(path):
        custom_module_path = os.path.join(SETTING_DIR, path)
        if not os.path.exists(custom_module_path):
            os.makedirs(custom_module_path)
        init_file_path = os.path.join(custom_module_path, '__init__.py')
        if not os.path.exists(init_file_path):
            open(init_file_path, 'a').close()


def create_airplay_process_manager(
        status_callback=None, media_callback=None, session_callback=None):
    configured_path = Setting.get(SettingProperty.AirPlayExecutable, '')
    auto_restart = bool(Setting.get(SettingProperty.AirPlayAutoRestart, 1))
    extra_arguments = Setting.get(SettingProperty.AirPlayExtraArguments, [])
    if not extra_arguments:
        extra_arguments = []
    if isinstance(extra_arguments, str):
        extra_arguments = shlex.split(
            extra_arguments, posix=sys.platform != 'win32')
    arguments = ['--log', '--macast-windowed']
    arguments.extend(str(argument) for argument in extra_arguments)
    hide_windows_ui = bool(
        Setting.get(SettingProperty.AirPlayHideWindowsUI, 1))
    app_data = os.environ.get('APPDATA', SETTING_DIR)
    return AirPlayProcessManager(
        executable=configured_path,
        arguments=arguments,
        working_dir=os.path.join(SETTING_DIR, 'AirPlay-Windows'),
        settings_path=os.path.join(
            app_data, 'AirPlay-Windows', 'settings.ini'),
        log_path=os.path.join(SETTING_DIR, 'airplay-windows.log'),
        receiver_name=Setting.get_friendly_name(),
        auto_restart=auto_restart,
        hide_until_mirror=hide_windows_ui,
        require_macast_window=True,
        media_callback=media_callback,
        session_callback=session_callback,
        status_callback=status_callback,
        env=Setting.get_system_env())


def create_bluetooth_audio_process_manager(state_callback=None):
    if sys.platform != 'win32':
        return None
    return BluetoothAudioProcessManager(
        working_dir=os.path.join(SETTING_DIR, 'BluetoothAudio'),
        state_callback=state_callback,
        env=Setting.get_system_env())


class Macast(App):
    AIRPLAY_RESUME_DELAY = 2.0

    if sys.platform == 'win32':
        ICON_MAP = ['assets/icon.ico',
                    'assets/menu_light_large.png',
                    'assets/menu_dark_large.png']
    else:
        ICON_MAP = ['assets/icon.png',
                    'assets/menu_light.png',
                    'assets/menu_dark.png']

    def __init__(self, renderer, protocol, lang=gettext.gettext,
                 airplay_manager=None):
        global _
        _ = lang
        # menu items
        self.toggle_menuitem = None
        self.setting_menuitem = None
        self.quit_menuitem = None
        self.ip_menuitem = None
        self.version_menuitem = None
        self.auto_check_update_menuitem = None
        self.start_at_login_menuitem = None
        self.menubar_icon_menuitem = None
        self.check_update_menuitem = None
        self.about_menuitem = None
        self.open_config_menuitem = None
        self.renderer_menuitem = None
        self.protocol_menuitem = None
        self.advanced_menuitem = None
        self.airplay_menuitem = None
        self.airplay_status_menuitem = None
        self.airplay_toggle_menuitem = None
        self.airplay_restart_menuitem = None
        self.airplay_show_menuitem = None
        self.airplay_now_playing_menuitem = None
        self.airplay_control_menuitems = []
        self.airplay_preemption_lock = threading.RLock()
        self.airplay_paused_by_dlna = False
        self.airplay_resume_timer = None
        self.bluetooth_audio_menuitem = None
        self.bluetooth_audio_status_menuitem = None
        self.bluetooth_audio_toggle_menuitem = None
        self.bluetooth_audio_restart_menuitem = None
        self.bluetooth_audio_device_menuitems = []
        self.bluetooth_owns_output = False

        if airplay_manager is None:
            airplay_manager = create_airplay_process_manager(
                self.on_airplay_status_changed,
                self.on_airplay_media_state_changed,
                self.on_airplay_session_started)
        else:
            airplay_manager.status_callback = self.on_airplay_status_changed
            airplay_manager.media_callback = self.on_airplay_media_state_changed
            airplay_manager.session_callback = self.on_airplay_session_started
        self.airplay_manager = airplay_manager
        self.bluetooth_audio_manager = \
            create_bluetooth_audio_process_manager(
                self.on_bluetooth_audio_state_changed)

        self.plugin_manager = MacastPluginManager(
            MacastPlugin(None, format_class_name(renderer), renderer, 'darwin,win32,linux'),
            MacastPlugin(None, format_class_name(protocol), protocol, 'darwin,win32,linux'))

        cherrypy.engine.subscribe('get_plugin_info', self.plugin_manager.get_info)

        # setting items
        self.setting_start_at_login = None
        self.setting_check = None
        self.setting_menubar_icon = 0
        self.setting_renderer = ''
        self.setting_protocol = ''
        self.init_setting()

        # init service
        self.service = Service(self.plugin_manager.get_renderer(self.setting_renderer),
                               self.plugin_manager.get_protocol(self.setting_protocol))

        icon_path = os.path.join(os.path.dirname(__file__), Macast.ICON_MAP[self.setting_menubar_icon])
        template = None if self.setting_menubar_icon == 0 else True
        self.copy_menuitem = None
        super(Macast, self).__init__("Macast",
                                     icon_path,
                                     self.build_app_menu(),
                                     template
                                     )
        cherrypy.engine.subscribe('start', self.service_start)
        cherrypy.engine.subscribe('stop', self.service_stop)
        cherrypy.engine.subscribe('renderer_start', self.renderer_start)
        cherrypy.engine.subscribe('renderer_av_stop', self.renderer_av_stop)
        cherrypy.engine.subscribe('renderer_av_uri', self.renderer_av_uri)
        cherrypy.engine.subscribe(
            'dlna_cast_requested', self.on_dlna_cast_requested)
        cherrypy.engine.subscribe('ssdp_update_ip', self.update_service_ip)
        cherrypy.engine.subscribe('app_notify', self.notification)
        self.start_cast()
        atexit.register(self.airplay_manager.stop)
        if self.bluetooth_audio_manager is not None:
            atexit.register(self.bluetooth_audio_manager.stop)
        if Setting.get(
                SettingProperty.AirPlayEnabled,
                1 if sys.platform == 'win32' else 0):
            self._start_airplay_on_launch()
        if self.bluetooth_audio_manager is not None and Setting.get(
                SettingProperty.BluetoothAudioEnabled, 0):
            self._start_bluetooth_audio_on_launch()
        logger.debug("Macast APP started")

    def build_app_menu(self):
        self.toggle_menuitem = MenuItem(_("Stop Cast"), self.on_toggle_service_click, key="p")
        self.setting_menuitem = MenuItem(_("Setting"), children=self.build_setting_menu())
        self.quit_menuitem = MenuItem(_("Quit"), self.quit, key="q")
        return [
            self.toggle_menuitem,
            None,
            self.setting_menuitem,
            self.quit_menuitem
        ]

    def build_setting_menu(self):
        ip_text = "/".join([ip for ip, _ in Setting.get_ip()])
        port = Setting.get_port()
        self.ip_menuitem = MenuItem("{}:{}".format(ip_text, port), enabled=False)
        self.version_menuitem = MenuItem(
            "{} v{}".format(Setting.get_friendly_name(), Setting.get_version()), enabled=False)
        self.auto_check_update_menuitem = MenuItem(_("Auto Check Updates"),
                                                   self.on_auto_check_update_click,
                                                   checked=self.setting_check)
        self.start_at_login_menuitem = MenuItem(_("Start At Login"),
                                                self.on_start_at_login_click,
                                                checked=self.setting_start_at_login)

        renderer_names = [r.title for r in self.plugin_manager.renderer_list]
        renderer_select = []
        if len(renderer_names) > 1:
            self.renderer_menuitem = MenuItem(_("Renderers"),
                                              children=App.build_menu_item_group(renderer_names,
                                                                                 self.on_renderer_change_click))
            renderer_select = [self.renderer_menuitem]
            for i in self.renderer_menuitem.children:
                if i.text == self.setting_renderer:
                    i.checked = True
                    break
            else:
                self.renderer_menuitem.children[0].checked = True

        protocol_names = [r.title for r in self.plugin_manager.protocol_list]
        protocol_select = []
        if len(protocol_names) > 1:
            self.protocol_menuitem = MenuItem(_("Protocols"),
                                              children=App.build_menu_item_group(protocol_names,
                                                                                 self.on_protocol_change_click))
            protocol_select = [self.protocol_menuitem]
            for i in self.protocol_menuitem.children:
                if i.text == self.setting_protocol:
                    i.checked = True
                    break
            else:
                self.protocol_menuitem.children[0].checked = True

        platform_options = []
        """To judge whether Macast was launched by scripts or by packaged app.
        """
        if sys.platform == 'darwin' and sys.executable.endswith("Contents/MacOS/python"):
            platform_options = [self.start_at_login_menuitem]
            # Reset StartAtLogin to prevent the user from turning off
            # this option from the system settings
            Setting.set_start_at_login(self.setting_start_at_login)
        elif sys.platform == 'win32' and "python" not in os.path.basename(sys.executable).lower():
            platform_options = [self.start_at_login_menuitem]
            Setting.set_start_at_login(self.setting_start_at_login)
        if sys.platform == 'darwin':
            self.menubar_icon_menuitem = MenuItem(_("Menubar Icon"),
                                                  children=App.build_menu_item_group([
                                                      _("AppIcon"),
                                                      _("Pattern"),
                                                  ], self.on_menubar_icon_change_click))
        else:
            self.menubar_icon_menuitem = MenuItem(_("Menubar Icon"),
                                                  children=App.build_menu_item_group([
                                                      _("AppIcon"),
                                                      _("PatternLight"),
                                                      _("PatternDark"),
                                                  ], self.on_menubar_icon_change_click))
        self.open_config_menuitem = MenuItem(_("Open Config Directory"), self.on_open_config_click)
        self.advanced_menuitem = MenuItem(_("Advanced Setting"),
                                          lambda _: self.open_browser('http://127.0.0.1:{}'.format(Setting.get_port())))
        self.check_update_menuitem = MenuItem(_("Check For Updates"), self.on_check_click)
        self.about_menuitem = MenuItem(_("Help"), self.on_about_click)
        self.airplay_menuitem = MenuItem(
            _("AirPlay Screen Mirroring"),
            children=self.build_airplay_menu())
        bluetooth_settings = []
        if self.bluetooth_audio_manager is not None:
            self.bluetooth_audio_menuitem = MenuItem(
                _("Bluetooth Audio Receiver"),
                children=self.build_bluetooth_audio_menu())
            bluetooth_settings = [self.bluetooth_audio_menuitem, None]

        self.menubar_icon_menuitem.items()[self.setting_menubar_icon].checked = True
        player_settings = self.service.renderer.renderer_setting.build_menu()
        if len(player_settings) > 0:
            player_settings.append(None)
        airplay_settings = [self.airplay_menuitem, None]

        return [self.version_menuitem, self.ip_menuitem] + \
               renderer_select + \
               protocol_select + \
               [None] + \
               player_settings + \
               bluetooth_settings + \
               airplay_settings + \
               [self.menubar_icon_menuitem, self.auto_check_update_menuitem,
                self.open_config_menuitem, self.advanced_menuitem] + \
               platform_options + \
               [None, self.check_update_menuitem, self.about_menuitem]

    def build_bluetooth_audio_menu(self):
        manager = self.bluetooth_audio_manager
        self.bluetooth_audio_status_menuitem = MenuItem(
            self.get_bluetooth_audio_status_text(),
            enabled=False)
        self.bluetooth_audio_toggle_menuitem = MenuItem(
            self.get_bluetooth_audio_toggle_text(),
            self.on_bluetooth_audio_toggle_click)
        self.bluetooth_audio_restart_menuitem = MenuItem(
            _("Restart Bluetooth Audio"),
            self.on_bluetooth_audio_restart_click,
            enabled=manager.is_running())
        device_items = []
        devices = manager.devices
        if not devices:
            device_items.append(MenuItem(
                _("No paired Bluetooth audio devices"),
                enabled=False))
        else:
            connected_id = manager.device_id
            for device in devices:
                item = MenuItem(
                    device["name"] or device["id"],
                    self.on_bluetooth_audio_device_click,
                    checked=(device["id"] == connected_id),
                    data=device)
                device_items.append(item)
        self.bluetooth_audio_device_menuitems = device_items
        open_bluetooth_settings = MenuItem(
            _("Open Windows Bluetooth Settings"),
            self.on_open_bluetooth_settings_click)
        refresh_item = MenuItem(
            _("Refresh Bluetooth Devices"),
            self.on_refresh_bluetooth_devices_click,
            enabled=manager.is_running())
        return [
            self.bluetooth_audio_status_menuitem,
            None,
            self.bluetooth_audio_toggle_menuitem,
            self.bluetooth_audio_restart_menuitem,
            refresh_item,
            open_bluetooth_settings,
            None,
        ] + device_items

    def get_bluetooth_audio_status_text(self):
        manager = self.bluetooth_audio_manager
        labels = {
            manager.STOPPED: _("Stopped"),
            manager.STARTING: _("Starting"),
            manager.READY: _("Ready"),
            manager.CONNECTING: _("Connecting"),
            manager.CONNECTED: _("Connected"),
            manager.ERROR: _("Error"),
            manager.MISSING: _("Executable not found"),
        }
        text = "{}: {}".format(
            _("Bluetooth Audio"),
            labels.get(manager.state, manager.state))
        if manager.state == manager.CONNECTED and manager.device_name:
            text += " ({})".format(manager.device_name)
        return text

    def get_bluetooth_audio_toggle_text(self):
        if self.bluetooth_audio_manager.is_running():
            return _("Stop Bluetooth Audio")
        return _("Start Bluetooth Audio")

    def build_airplay_menu(self):
        media = self.airplay_manager.media_state
        controls_enabled = self.is_airplay_active() and media.get("control")
        self.airplay_status_menuitem = MenuItem(
            self.get_airplay_status_text(),
            enabled=False)
        self.airplay_now_playing_menuitem = MenuItem(
            self.get_airplay_now_playing_text(media),
            enabled=False)
        self.airplay_toggle_menuitem = MenuItem(
            self.get_airplay_toggle_text(),
            self.on_airplay_toggle_click)
        self.airplay_restart_menuitem = MenuItem(
            _("Restart AirPlay"),
            self.on_airplay_restart_click,
            enabled=self.airplay_manager.is_running())
        self.airplay_show_menuitem = MenuItem(
            _("Show Mirror Window"),
            self.on_airplay_show_click,
            enabled=(self.airplay_manager.is_running()
                     and sys.platform == 'win32'))
        media_items = [
            MenuItem(_("Previous Track"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="previtem"),
            MenuItem(_("Play"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="play"),
            MenuItem(_("Pause"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="pause"),
            MenuItem(_("Toggle Play/Pause"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="playpause"),
            MenuItem(_("Next Track"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="nextitem"),
            MenuItem(_("Volume Up"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="volumeup"),
            MenuItem(_("Volume Down"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="volumedown"),
            MenuItem(_("Toggle Mute"), self.on_airplay_media_click,
                     enabled=controls_enabled, data="mutetoggle"),
        ]
        self.airplay_control_menuitems = media_items
        return [
            self.airplay_status_menuitem,
            self.airplay_now_playing_menuitem,
            None,
            self.airplay_toggle_menuitem,
            self.airplay_restart_menuitem,
            self.airplay_show_menuitem,
            None,
        ] + media_items

    def get_airplay_now_playing_text(self, media=None):
        media = media or self.airplay_manager.media_state
        title = media.get("title", "").strip()
        artist = media.get("artist", "").strip()
        if not title and not artist:
            return _("AirPlay: No audio metadata")
        text = title
        if artist:
            text = "{} — {}".format(text, artist) if text else artist
        duration_ms = int(media.get("duration_ms", 0))
        elapsed_ms = int(media.get("elapsed_ms", 0))
        if duration_ms > 0:
            text = "{}  [{}/{}]".format(
                text, self.format_media_time(elapsed_ms),
                self.format_media_time(duration_ms))
        return text

    @staticmethod
    def format_media_time(milliseconds):
        seconds = max(0, int(milliseconds) // 1000)
        return "{}:{:02d}".format(seconds // 60, seconds % 60)

    def get_airplay_status_text(self):
        state_labels = {
            AirPlayProcessManager.STARTING: _("Starting"),
            AirPlayProcessManager.RUNNING: _("Running"),
            AirPlayProcessManager.RESTARTING: _("Restarting"),
            AirPlayProcessManager.STOPPED: _("Stopped"),
            AirPlayProcessManager.MISSING: _("Executable not found"),
            AirPlayProcessManager.ERROR: _("Error"),
        }
        state = self.airplay_manager.state
        return "{}: {}".format(
            _("AirPlay"),
            state_labels.get(state, state))

    def get_airplay_toggle_text(self):
        if self.is_airplay_active():
            return _("Stop AirPlay")
        return _("Start AirPlay")

    def is_airplay_active(self):
        return self.airplay_manager.state in [
            AirPlayProcessManager.STARTING,
            AirPlayProcessManager.RUNNING,
            AirPlayProcessManager.RESTARTING,
        ]

    def init_setting(self):
        self.setting_start_at_login = Setting.get(SettingProperty.StartAtLogin, 0)
        self.setting_check = Setting.get(SettingProperty.CheckUpdate, 1)
        self.setting_menubar_icon = Setting.get(SettingProperty.MenubarIcon, 1 if sys.platform == 'darwin' else 0)
        self.setting_renderer = Setting.get(SettingProperty.Macast_Renderer, 'MPV')
        self.setting_protocol = Setting.get(SettingProperty.Macast_Protocol, 'DLNA')
        if self.setting_check:
            threading.Thread(target=self.check_update,
                             kwargs={
                                 'verbose': False
                             },
                             daemon=True,
                             name="CHECKUPDATE_THREAD").start()

    def stop_cast(self):
        self.service.stop()

    def start_cast(self):
        self.service.run_async()

    def check_update(self, verbose=True):
        release_url = 'https://github.com/xfangfang/Macast/releases/latest'
        api_url = 'https://api.github.com/repos/xfangfang/Macast/releases/latest'
        try:
            res = json.loads(requests.get(api_url).text)
            online_version = re.findall(r'(\d+\.*\d+)', res['tag_name'])[0]

            logger.info("tag_name: {}".format(res['tag_name']))

            if float(Setting.get_version()) < float(online_version):
                self.dialog(_("Macast New Update {}").format(res['tag_name']),
                            lambda: self.open_browser(release_url),
                            ok="Update")
            else:
                if verbose:
                    self.notification("Macast", _("You're up to date."))
        except Exception as e:
            logger.error("get update info error: {}".format(e))

    # The followings are the callback function of program event

    def update_service_status(self):
        if Setting.is_service_running():
            self.toggle_menuitem.text = _('Stop Cast')
        else:
            self.toggle_menuitem.text = _('Start Cast')
        self.update_menu()

    def on_airplay_status_changed(self, state, message):
        logger.info("AirPlay status: %s (%s)", state, message)
        if self.airplay_status_menuitem is None:
            return
        running = state in [
            AirPlayProcessManager.STARTING,
            AirPlayProcessManager.RUNNING,
            AirPlayProcessManager.RESTARTING,
        ]
        self.airplay_status_menuitem.text = self.get_airplay_status_text()
        self.airplay_toggle_menuitem.text = self.get_airplay_toggle_text()
        self.airplay_restart_menuitem.enabled = running
        self.airplay_show_menuitem.enabled = running and sys.platform == 'win32'
        self.update_airplay_media_controls()
        self.update_menu()

    def on_airplay_media_state_changed(self, media):
        with self.airplay_preemption_lock:
            paused_by_dlna = self.airplay_paused_by_dlna
        if paused_by_dlna and not media.get("control"):
            self.cancel_airplay_resume()
        if self.airplay_now_playing_menuitem is None:
            return
        self.airplay_now_playing_menuitem.text = \
            self.get_airplay_now_playing_text(media)
        self.update_airplay_media_controls(media)
        self.update_menu()

    def on_airplay_session_started(self):
        """The latest AirPlay sender preempts DLNA and Bluetooth audio."""
        self.cancel_airplay_resume()
        with self.airplay_preemption_lock:
            self.airplay_paused_by_dlna = False
        self.bluetooth_owns_output = False
        bluetooth_manager = getattr(
            self, 'bluetooth_audio_manager', None)
        if bluetooth_manager is not None and \
                bluetooth_manager.is_connected():
            bluetooth_manager.disconnect_device()
        self.stop_active_dlna()

    def stop_active_dlna(self):
        protocol = self.service.protocol
        uri = protocol.get_state_url()
        transport = protocol.get_state_transport_state()
        if not uri or transport in ("STOPPED", "NO_MEDIA_PRESENT"):
            return
        logger.info("AirPlay sender preempted active DLNA playback")
        self.service.renderer.set_media_stop()
        protocol.set_state_stop()
        cherrypy.engine.publish('renderer_av_stop')

    def on_dlna_cast_requested(self, uri):
        """The latest DLNA sender preempts AirPlay and Bluetooth audio."""
        self.cancel_airplay_resume()
        self.bluetooth_owns_output = False
        bluetooth_manager = getattr(
            self, 'bluetooth_audio_manager', None)
        if bluetooth_manager is not None and \
                bluetooth_manager.is_connected():
            bluetooth_manager.disconnect_device()
        self.suspend_or_preempt_airplay("DLNA", uri)

    def suspend_or_preempt_airplay(self, source, detail=""):
        media = self.airplay_manager.media_state
        has_airplay_media = bool(
            media.get("title") or media.get("artist") or
            media.get("duration_ms", 0))
        if media.get("control") and has_airplay_media:
            if self.airplay_manager.send_media_command("pause"):
                with self.airplay_preemption_lock:
                    self.airplay_paused_by_dlna = True
                logger.warning(
                    "%s sender paused active AirPlay audio", source)
                return True
        if self.airplay_manager.send_media_command("preempt"):
            logger.warning(
                "%s sender preempted an active AirPlay session: %s",
                source, detail)
        return False

    def on_bluetooth_audio_state_changed(self, snapshot):
        state = snapshot.get("state", "")
        manager = self.bluetooth_audio_manager
        if self.bluetooth_audio_status_menuitem is not None:
            self.bluetooth_audio_status_menuitem.text = \
                self.get_bluetooth_audio_status_text()
            self.bluetooth_audio_toggle_menuitem.text = \
                self.get_bluetooth_audio_toggle_text()
            self.bluetooth_audio_restart_menuitem.enabled = \
                manager.is_running()
            for item in self.bluetooth_audio_device_menuitems:
                device = item.data or {}
                item.checked = device.get("id") == manager.device_id

        if state == manager.CONNECTED and not self.bluetooth_owns_output:
            self.bluetooth_owns_output = True
            self.cancel_airplay_resume()
            self.suspend_or_preempt_airplay(
                "Bluetooth", manager.device_name)
            self.stop_active_dlna()
        elif state in {
                manager.READY, manager.STOPPED, manager.ERROR,
                manager.MISSING} and self.bluetooth_owns_output:
            self.bluetooth_owns_output = False
            self.schedule_airplay_resume()

        if state == manager.ERROR and manager.message:
            logger.warning("Bluetooth audio error: %s", manager.message)
        if self.bluetooth_audio_menuitem is not None:
            self.update_menu()

    def cancel_airplay_resume(self):
        with self.airplay_preemption_lock:
            timer = self.airplay_resume_timer
            self.airplay_resume_timer = None
            if timer is not None:
                timer.cancel()

    def schedule_airplay_resume(self):
        with self.airplay_preemption_lock:
            if not self.airplay_paused_by_dlna:
                return
            bluetooth_manager = getattr(
                self, 'bluetooth_audio_manager', None)
            if bluetooth_manager is not None and \
                    bluetooth_manager.is_connected():
                return
            if self.airplay_resume_timer is not None:
                self.airplay_resume_timer.cancel()
            timer = threading.Timer(
                self.AIRPLAY_RESUME_DELAY,
                self.resume_airplay_after_dlna)
            timer.daemon = True
            self.airplay_resume_timer = timer
            timer.start()

    def resume_airplay_after_dlna(self):
        with self.airplay_preemption_lock:
            self.airplay_resume_timer = None
            if not self.airplay_paused_by_dlna:
                return
        bluetooth_manager = getattr(
            self, 'bluetooth_audio_manager', None)
        if bluetooth_manager is not None and \
                bluetooth_manager.is_connected():
            return
        success = False
        for command in ("play", "playpause", "play"):
            if self.airplay_manager.send_media_command(command):
                success = True
                break
            time.sleep(0.5)
        with self.airplay_preemption_lock:
            self.airplay_paused_by_dlna = False
        if success:
            logger.warning("AirPlay audio resumed after DLNA playback ended")
        else:
            logger.warning(
                "AirPlay audio could not resume after DLNA playback ended")

    def update_airplay_media_controls(self, media=None):
        media = media or self.airplay_manager.media_state
        enabled = self.is_airplay_active() and bool(media.get("control"))
        for item in self.airplay_control_menuitems:
            item.enabled = enabled

    def start_airplay(self, auto=False):
        if self.airplay_manager.start():
            Setting.set(SettingProperty.AirPlayEnabled, 1)
            return True
        if not auto:
            self.notification(
                _("AirPlay"),
                self.airplay_manager.message)
        return False

    def _start_airplay_on_launch(self):
        """Start AirPlay after the tray/service bootstrap has settled.

        A quick Macast restart can overlap with the previous AirPlay child
        releasing its single-instance mutex. Starting this from a short-lived
        worker lets the manager wait for that hand-off without blocking the
        tray icon and CherryPy bootstrap.
        """
        def delayed_start():
            time.sleep(0.75)
            if Setting.get(SettingProperty.AirPlayEnabled, 0):
                self.start_airplay(auto=True)

        threading.Thread(
            target=delayed_start,
            name="AIRPLAY_AUTOSTART",
            daemon=True).start()

    def stop_airplay(self):
        self.airplay_manager.stop()
        Setting.set(SettingProperty.AirPlayEnabled, 0)

    def _start_bluetooth_audio_on_launch(self):
        def delayed_start():
            time.sleep(1.0)
            if Setting.get(SettingProperty.BluetoothAudioEnabled, 0):
                self.start_bluetooth_audio(auto=True)

        threading.Thread(
            target=delayed_start,
            name="BLUETOOTH_AUDIO_AUTOSTART",
            daemon=True).start()

    def start_bluetooth_audio(self, auto=False):
        manager = self.bluetooth_audio_manager
        if manager is None:
            return False
        if not manager.start():
            if not auto:
                self.notification(_("Bluetooth Audio"), manager.message)
            return False
        Setting.set(SettingProperty.BluetoothAudioEnabled, 1)
        configured_device = Setting.get(
            SettingProperty.BluetoothAudioDevice, "")
        if configured_device and not manager.is_connected():
            device_name = ""
            for device in manager.devices:
                if device["id"] == configured_device:
                    device_name = device["name"]
                    break

            def connect_saved_device():
                if not manager.connect_device(
                        configured_device, device_name):
                    logger.warning(
                        "Unable to reconnect Bluetooth audio: %s",
                        manager.message)

            threading.Thread(
                target=connect_saved_device,
                name="BLUETOOTH_AUDIO_CONNECT",
                daemon=True).start()
        self.setting_menuitem.children = self.build_setting_menu()
        self.set_menu(self.menu)
        return True

    def stop_bluetooth_audio(self):
        manager = self.bluetooth_audio_manager
        if manager is None:
            return False
        manager.stop()
        Setting.set(SettingProperty.BluetoothAudioEnabled, 0)
        self.setting_menuitem.children = self.build_setting_menu()
        self.set_menu(self.menu)
        return True

    def service_start(self):
        """This function is called every time the DLNA service is started.
        Displays a notification reminding the user that the service has started
        """
        logger.info("service_start")
        if self.platform is Platform.Win32:
            msg = _("running at task bar")
        elif self.platform is Platform.Darwin:
            msg = _("running at menu bar")
        else:
            msg = _("running at desktop panel")
        if self.platform == Platform.Darwin:
            self.notification(_("Macast is hidden"), msg, sound=False)
        else:
            # Pystray may fail to send notifications due to incomplete initialization
            # during the startup, so wait a moment
            threading.Thread(target=lambda: (
                time.sleep(2),
                self.notification(_("Macast is hidden"), msg, sound=False),
            )).start()
        self.update_service_status()

    def service_stop(self):
        """This function is called every time the DLNA service is stopped.
        """
        logger.info("service_stop")
        self.update_service_status()

    def update_service_ip(self):
        """When the IP or port of the device changes,
        call this function to refresh the device address on the menu
        """
        logger.info("ssdp_update_ip")
        if self.ip_menuitem is not None:
            ip_text = "/".join([ip for ip, _ in Setting.get_ip()])
            port = Setting.get_port()
            self.ip_menuitem.text = "{}:{}".format(ip_text, port)
        self.version_menuitem.text = "{} v{}".format(Setting.get_friendly_name(), Setting.get_version())
        self.update_menu()

    def renderer_av_stop(self):
        logger.info("renderer_av_stop")
        self.schedule_airplay_resume()
        if self.copy_menuitem:
            self.remove_menu_item_by_id(self.copy_menuitem.id)
        self.copy_menuitem = None

    def renderer_start(self):
        pass

    def renderer_av_uri(self, uri):
        logger.info("renderer_av_uri: " + uri)
        if self.copy_menuitem is not None:
            self.copy_menuitem.callback = lambda _: pyperclip.copy(uri)
            return
        self.copy_menuitem = MenuItem(
            _("Copy Video URI"),
            key="c",
            callback=lambda _: pyperclip.copy(uri))
        self.append_menu_item_after(self.toggle_menuitem.id, self.copy_menuitem)

    # The followings are the callback function of menu click

    def on_protocol_change_click(self, item):
        protocol_config = self.plugin_manager.protocol_list[item.data]
        self.stop_cast()
        # todo 生成新的 uuid
        self.service.protocol = protocol_config.get_instance()
        Setting.set(SettingProperty.Macast_Protocol, protocol_config.title)
        self.setting_protocol = protocol_config.title
        self.setting_menuitem.children = self.build_setting_menu()
        # reload menu
        self.set_menu(self.menu)
        self.start_cast()
        cherrypy.engine.publish('app_notify', _('Info'), _('Change Protocol to {}.').format(protocol_config.title))

    def on_renderer_change_click(self, item):
        renderer_config = self.plugin_manager.renderer_list[item.data]
        self.service.renderer = renderer_config.get_instance()
        Setting.set(SettingProperty.Macast_Renderer, renderer_config.title)
        self.setting_renderer = renderer_config.title
        self.setting_menuitem.children = self.build_setting_menu()
        # reload menu
        self.set_menu(self.menu)
        cherrypy.engine.publish('app_notify', _('Info'), _('Change Renderer to {}.').format(renderer_config.title))

    def on_open_config_click(self, item):
        self.open_directory(SETTING_DIR)

    def on_check_click(self, item):
        threading.Thread(target=self.check_update,
                         daemon=True,
                         name="CHECKUPDATE_M_THREAD").start()

    def on_auto_check_update_click(self, item):
        item.checked = not item.checked
        Setting.set(SettingProperty.CheckUpdate,
                    1 if item.checked else 0)

    def on_start_at_login_click(self, item):
        res = Setting.set_start_at_login(not item.checked)
        if res[0] == 0:
            item.checked = not item.checked
            Setting.set(SettingProperty.StartAtLogin,
                        1 if item.checked else 0)
        else:
            self.notification(_("Error"), _(res[1]))

    def on_about_click(self, _):
        self.open_browser('http://127.0.0.1:{}?page=4'.format(Setting.get_port()))

    def on_toggle_service_click(self, item):
        if Setting.is_service_running():
            self.stop_cast()
        else:
            self.start_cast()

    def on_airplay_toggle_click(self, item):
        if self.is_airplay_active():
            self.stop_airplay()
        else:
            self.start_airplay()

    def on_airplay_restart_click(self, item):
        if self.airplay_manager.restart():
            Setting.set(SettingProperty.AirPlayEnabled, 1)
        else:
            self.notification(
                _("AirPlay"),
                self.airplay_manager.message)

    def on_airplay_show_click(self, item):
        if not self.airplay_manager.show_window():
            self.notification(
                _("AirPlay"),
                _("The AirPlay receiver window cannot be shown here."))

    def on_airplay_media_click(self, item):
        if not self.airplay_manager.send_media_command(item.data):
            self.notification(
                _("AirPlay"),
                _("AirPlay media control is unavailable."))

    def on_bluetooth_audio_toggle_click(self, item):
        manager = self.bluetooth_audio_manager
        if manager.is_running():
            self.stop_bluetooth_audio()
        else:
            threading.Thread(
                target=self.start_bluetooth_audio,
                name="BLUETOOTH_AUDIO_START",
                daemon=True).start()

    def on_bluetooth_audio_restart_click(self, item):
        manager = self.bluetooth_audio_manager

        def restart():
            if not manager.restart():
                self.notification(
                    _("Bluetooth Audio"), manager.message)
                return
            self.setting_menuitem.children = self.build_setting_menu()
            self.set_menu(self.menu)

        threading.Thread(
            target=restart,
            name="BLUETOOTH_AUDIO_RESTART",
            daemon=True).start()

    def on_refresh_bluetooth_devices_click(self, item):
        manager = self.bluetooth_audio_manager

        def refresh():
            manager.refresh_devices()
            self.setting_menuitem.children = self.build_setting_menu()
            self.set_menu(self.menu)

        threading.Thread(
            target=refresh,
            name="BLUETOOTH_AUDIO_REFRESH",
            daemon=True).start()

    def on_bluetooth_audio_device_click(self, item):
        manager = self.bluetooth_audio_manager
        device = item.data

        def connect():
            if manager.is_connected():
                manager.disconnect_device()
            if manager.connect_device(device["id"], device["name"]):
                Setting.set(
                    SettingProperty.BluetoothAudioDevice, device["id"])
                Setting.set(
                    SettingProperty.BluetoothAudioEnabled, 1)
            else:
                self.notification(
                    _("Bluetooth Audio"), manager.message)
            self.setting_menuitem.children = self.build_setting_menu()
            self.set_menu(self.menu)

        threading.Thread(
            target=connect,
            name="BLUETOOTH_AUDIO_DEVICE",
            daemon=True).start()

    def on_open_bluetooth_settings_click(self, item):
        if sys.platform == 'win32':
            os.startfile("ms-settings:bluetooth")

    def on_menubar_icon_change_click(self, item):
        for i in self.menubar_icon_menuitem.items():
            i.checked = False
        item.checked = True
        Setting.set(SettingProperty.MenubarIcon, item.data)
        icon_path = os.path.join(os.path.dirname(__file__), Macast.ICON_MAP[item.data])
        template = None if item.data == 0 else True
        self.update_icon(icon_path, template)

    def quit(self, item):
        if Setting.is_service_running():
            self.stop_cast()
        self.airplay_manager.stop()
        self.airplay_manager.status_callback = None
        if self.bluetooth_audio_manager is not None:
            self.bluetooth_audio_manager.stop()
            self.bluetooth_audio_manager.state_callback = None
        super(Macast, self).quit(item)


def gui(renderer=None, protocol=None, lang=gettext.gettext,
        airplay_manager=None):
    if renderer is None:
        renderer = MPVRenderer(lang, Setting.mpv_default_path)
    if protocol is None:
        protocol = DLNAProtocol()
    Macast(renderer, protocol, lang, airplay_manager).start()


def cli(renderer=None, protocol=None):
    if renderer is None:
        renderer = MPVRenderer(path=Setting.mpv_default_path)
    if protocol is None:
        protocol = DLNAProtocol()
    airplay_manager = create_airplay_process_manager()
    if Setting.get(
            SettingProperty.AirPlayEnabled,
            1 if sys.platform == 'win32' else 0):
        airplay_manager.start()
    try:
        Service(renderer, protocol).run()
    finally:
        airplay_manager.stop()
