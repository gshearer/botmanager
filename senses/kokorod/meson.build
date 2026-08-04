# kokorod — the Kokoro TTS server behind the robot's voice.
#
# Guarded on purpose: sherpa-onnx is an operator-installed distro package, not
# a submodule, so a peer's checkout without it must still build green. When the
# dependency is absent this subdir declares nothing and the rest of the tree is
# unaffected — `ninja` simply produces no kokorod.
sherpa_dep = dependency('sherpa-onnx', required : false)

if sherpa_dep.found()
  kokorod_exe = executable('kokorod',
    'kokorod.c',
    dependencies : [sherpa_dep, cc.find_library('m', required : false)],
    install : false,
  )

else
  message('sherpa-onnx not found — skipping kokorod (install the sherpa-onnx package to build it)')
endif
