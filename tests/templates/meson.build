template_files = [
  '__init__.py',
  'clipboard.py',
  'email.py',
  'globalshortcuts.py',
  'inputcapture.py',
  'remotedesktop.py',
]
foreach template_file : template_files
  configure_file(
    input: template_file,
    output: template_file,
    copy: true,
    install: false
  )
endforeach
