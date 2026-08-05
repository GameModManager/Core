# Fugue base pack

Files in this directory are copied from the Fugue icon set
(`fugue-icons-3.5.6/icons/`, by Yusuke Kamiyamane, CC-BY-3.0) and RENAMED from
Fugue's native filenames to the app's logical icon keys, so the central
IconManager resolver can look up `<key>.png` in any pack directory with no
per-pack manifest.

The resolver consults this pack last (after the active theme/pack and the
system icon theme), making it the always-on safety net.

Key -> Fugue source mapping:

- list-add -> plus-circle.png
- list-remove -> minus-circle.png
- edit-delete / dialog-cancel -> cross-circle.png
- edit-clear -> color--minus.png
- go-up / go-top -> arrow-090.png
- go-down / go-bottom -> arrow-270.png
- merge -> arrow-merge.png
- folder -> folder.png
- document-new -> document--plus.png
- document-edit -> document--pencil.png
- document-open-folder -> folder-open.png
- color-picker -> color.png
- dialog-ok -> tick-circle.png
- dialog-information -> information.png
- download -> download.png
- media-playback-pause -> control-pause.png
- media-playback-start -> control.png
- text-html -> document-globe.png
- preferences-other / preferences-system -> gear.png
- view-sort -> sort.png
- computer -> computer.png
- root-dir -> root-dir.png
- archive -> box.png
- update-available -> arrow-circle.png

App-semantic icons (conflict-*, plugin-*, proton, gmm-logo) have no Fugue
equivalent; they ship as bundled assets in ../.. and are overridden by the
"mo2" pack.
