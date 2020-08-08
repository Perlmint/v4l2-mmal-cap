import sys
import subprocess
import os
import os.path
import xbmcplugin as plugin
import xbmcgui as gui
import xbmcaddon
import xbmc
import urlparse
import urllib

base_url = sys.argv[0]
handle = int(sys.argv[1])
args = urlparse.parse_qs(sys.argv[2][1:])

addon = xbmcaddon.Addon()
PLUGIN_PATH = addon.getAddonInfo("path")

CAPTURE_PATH = os.path.join(PLUGIN_PATH, 'resources', 'bin', 'v4l2-mmal-cap')
CAPTURE_SAVE_PATH = addon.getSetting('capture_path')
CAPTURE_DEVICE = addon.getSetting('capture_device')

def build_url(**query):
    return '{}?{}'.format(base_url, urllib.urlencode(query))

def main_menu():
    plugin.setContent(handle, 'pictures')

    url = build_url(action='capture')
    item = gui.ListItem(addon.getLocalizedString(32002))
    plugin.addDirectoryItem(handle, url, item, False)

    url = build_url(action='album')
    item = gui.ListItem(addon.getLocalizedString(32003))
    plugin.addDirectoryItem(handle, url, item, True)

    plugin.endOfDirectory(handle)

def album():
    plugin.setPluginCategory(handle, 'Album')
    plugin.setContent(handle, 'pictures')
    for filename in os.listdir(CAPTURE_SAVE_PATH):
        full_path = os.path.join(CAPTURE_SAVE_PATH, filename)
        item = gui.ListItem(filename, iconImage=full_path, thumbnailImage=full_path)
        item.addContextMenuItems([
            ('Remove', 'xbmc.RunPlugin({})'.format(build_url(action='remove', file=filename)))
        ])
        plugin.addDirectoryItem(handle, full_path, item, False)
    
    plugin.endOfDirectory(handle)


action = args.get('action', None)

if __name__ == '__main__':
    print(action)
    if action is None:
        main_menu()
    elif action[0] == 'capture':
        p = subprocess.Popen([CAPTURE_PATH, CAPTURE_DEVICE], stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=CAPTURE_SAVE_PATH)
        ret = p.wait()
        if ret != 0:
            se = p.stderr.read()
            gui.Dialog().ok("RPi Capture", "Failed to capture from {}".format(CAPTURE_DEVICE), se)
        else:
            so = p.stdout.read()
            xbmc.executebuiltin('ShowPicture({})'.format(so.strip()))
    elif action[0] == 'album':
        album()
    elif action[0] == 'remove':
        file = args.get('file', None)
        full_path = os.path.join(CAPTURE_SAVE_PATH, file[0])
        os.remove(full_path)
