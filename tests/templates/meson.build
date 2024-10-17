template_files = [
  '__init__.py',
  'clipboard.py',
  'email.py',
  'geoclue2.py',
  'globalshortcuts.py',
  'inputcapture.py',
  'remotedesktop.py',
  'usb.py',
]
foreach template_file : template_files
  configure_file(
    input: template_file,
    output: template_file,
    copy: true,
    install: false
  )
endforeach
