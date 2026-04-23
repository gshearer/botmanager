inference_include = include_directories('.')

inference_sources = [
  'inference.c',
  'llm.c',
  'llm_cmd.c',
  'knowledge.c',
  'knowledge_cmd.c',
  'knowledge_file.c',
  'acquire.c',
  'acquire_html.c',
  'acquire_cmd.c',
  'acquire_reactive.c',
  'acquire_digest.c',
  'acquire_feed.c',
]

inference_plugin = shared_library('inference',
  inference_sources,
  include_directories : [
    inc_include,
    inference_include,
    include_directories('../service/searxng'),
  ],
  dependencies : [curl_dep, json_c_dep, m_dep, xml2_dep],
  override_options : ['b_lundef=false'],
  install : true,
)
