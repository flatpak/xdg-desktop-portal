template_files = [
  '__init__.py',
  'email.py',
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
