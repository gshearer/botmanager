# kokorod — the Kokoro TTS server behind the robot's voice.
#
# Guarded on purpose: sherpa-onnx is an operator-installed distro package, not
# a submodule, so a peer's checkout without it must still build green. When the
# dependency is absent this subdir declares nothing and the rest of the tree is
# unaffected — `ninja` simply produces no kokorod.
#
# ⚠ cargs is guarded for the same reason and is not ours: sherpa-onnx's `.pc`
# puts `-lcargs` in `Libs:` unconditionally while the package does not depend
# on it, so a host carrying one and not the other fails at kokorod's LINK step
# and takes the whole tree's build down with it. kokorod never calls cargs.
# `plugins/method/reachy/BRINGUP.md §3.2` carries the install line.
sherpa_dep = dependency('sherpa-onnx', required : false)
cargs_lib = cc.find_library('cargs', required : false)

if sherpa_dep.found() and cargs_lib.found()
  kokorod_exe = executable('kokorod',
    'kokorod.c',
    dependencies : [sherpa_dep, cc.find_library('m', required : false)],
    install : false,
  )

elif sherpa_dep.found()
  message('cargs not found — skipping kokorod (sherpa-onnx links -lcargs: pacman -S cargs)')

else
  message('sherpa-onnx not found — skipping kokorod (install the sherpa-onnx package to build it)')
endif
