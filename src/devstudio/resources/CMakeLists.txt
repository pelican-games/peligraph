# The Studio's own resources. The proxy target carries the embedded bytes so
# every target that compiles a view source -- the application and the scene
# tests that compile the same .cpp files directly -- can link the same table.
b_embed_proxy_target(pelican_studio_view pelican_studio_resources)

b_embed(pelican_studio_resources graph_socket_styles.json)
b_embed(pelican_studio_resources studio_themes.json)
