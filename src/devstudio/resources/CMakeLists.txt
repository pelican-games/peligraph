# The Studio's own resources. The proxy target carries the embedded bytes so
# every target that compiles a view source -- the application and the scene
# tests that compile the same .cpp files directly -- can link the same table.
b_embed_proxy_target(pelican_studio_view pelican_studio_resources)

b_embed(pelican_studio_resources graph_socket_styles.json)
b_embed(pelican_studio_resources studio_themes.json)
# F-3: the fragment shader a new fullscreen pass starts from. The Pass Editor
# fills in one declaration per input and writes it into the project.
b_embed(pelican_studio_resources fullscreen_pass.frag.in)
# G-3: the other two starting points "New..." offers -- deferred lighting,
# which reads a G-buffer and the scene lights, and a tone map.
b_embed(pelican_studio_resources fullscreen_lighting_pass.frag.in)
b_embed(pelican_studio_resources fullscreen_tonemap_pass.frag.in)
