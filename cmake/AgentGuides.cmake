# Generate standard C++ string data without relying on source-file encodings
# or raw-string delimiters. Explicit lengths preserve every byte.
function(memix_embed_text path output)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
  file(READ "${path}" encoded_bytes HEX)
  string(LENGTH "${encoded_bytes}" hex_length)
  math(EXPR byte_length "${hex_length} / 2")
  set(literals "      \"\"")
  set(offset 0)
  while(offset LESS hex_length)
    # Keep generated lines short; each adjacent literal contains 32 bytes.
    string(SUBSTRING "${encoded_bytes}" ${offset} 64 chunk)
    string(REGEX REPLACE "(..)" "\\\\x\\1" chunk "${chunk}")
    string(APPEND literals "\n      \"${chunk}\"")
    math(EXPR offset "${offset} + 64")
  endwhile()
  set(${output} "std::string_view{\n${literals}, ${byte_length}}" PARENT_SCOPE)
endfunction()

function(memix_generate_agent_guides source_dir output_header)
  set(memix_agent_entries "")
  foreach(topic IN ITEMS guide recall save compose attach recovery)
    memix_embed_text("${source_dir}/${topic}.md" text)
    string(APPEND memix_agent_entries "  {.topic = \"${topic}\", .text = ${text}},\n")
  endforeach()
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/agent_guides.hpp.in"
    "${output_header}" @ONLY NEWLINE_STYLE LF)
endfunction()

function(memix_generate_skill_assets source_dir output_header)
  memix_embed_text("${source_dir}/SKILL.md" memix_skill_markdown)
  memix_embed_text("${source_dir}/agents/openai.yaml" memix_skill_openai)
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/skill_assets.hpp.in"
    "${output_header}" @ONLY NEWLINE_STYLE LF)
endfunction()
