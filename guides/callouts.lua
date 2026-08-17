-- Render the guides' "Try it yourself!" disclosures as a boxed callout in the PDF.
--
-- In the Markdown they are GitHub disclosures, because the .md files ship to participants and are
-- read on GitHub, where this collapses:
--
--     <details>
--     <summary><b>Try it yourself! — build the controller and watch it run</b></summary>
--
--     ...the exercise...
--     </details>
--
-- LaTeX has no such thing, and pandoc DROPS raw HTML entirely when writing LaTeX -- tags and all.
-- Left alone, the summary line therefore lands in the PDF as an ordinary unstyled paragraph, with
-- nothing marking the exercise at all (the <b> goes the same way as the <details>). This filter
-- turns the pair into the `tryit` environment that template.tex defines, and rebuilds the summary
-- as real Strong inlines so it survives as the box's heading.
--
-- ONLY disclosures whose summary starts with "Try it yourself" are boxed. The guides use <details>
-- for one other thing -- "Show the two finished reactions", the answer reveal in doca-pcc's Check
-- your answer -- and an answer is not an exercise; boxing it would advertise the spoiler it exists
-- to hide. Everything else keeps pandoc's default behaviour, which is what it has always had.
-- There are two ways in, because the guides box two different things:
--
--   <details>/<summary>Try it yourself…      an exercise. Collapses on GitHub; add `open` to
--                                            <details> to keep it expanded there.
--   <div class="tryit"> … </div>             a plain callout with no disclosure, for a box that
--                                            is not a walkthrough (a standing reminder, say).
--
-- Both are HTML on purpose. A pandoc fenced div (`::: tryit`) would be tidier in the source, but
-- GitHub has no fenced-div support and would print the literal ":::" lines to participants -- and
-- these .md files ship. A <div> with a class costs nothing there: GitHub drops the attribute and
-- renders the contents as ordinary Markdown, exactly as if the wrapper were not there.
--
-- NOTE for either form: leave a BLANK LINE after the opening tag and before the closing one, or
-- neither GitHub nor pandoc parses the Markdown inside as Markdown.
local TRYIT = "Try it yourself"

-- <div class="tryit"> … </div>. Pandoc parses this into a real Div carrying the class, so the
-- wrapper is all this needs to be.
function Div(el)
  for _, class in ipairs(el.classes) do
    if class == "tryit" then
      local out = {pandoc.RawBlock("latex", "\\begin{tryit}")}
      for _, block in ipairs(el.content) do
        out[#out + 1] = block
      end
      out[#out + 1] = pandoc.RawBlock("latex", "\\end{tryit}")
      return out
    end
  end
end

-- Trimmed text of a raw HTML block; nil for any other block.
local function html(block)
  if block and block.t == "RawBlock" and block.format == "html" then
    return (block.text:gsub("^%s*(.-)%s*$", "%1"))
  end
end

-- The summary's inlines with the raw <b>/<strong> tags dropped, or nil if this is not a summary
-- body. Pandoc parses `<summary><b>text</b></summary>` as three blocks -- the two tags as RawBlocks
-- and the text between them as a Plain -- so the title arrives on its own.
local function summary_inlines(block)
  if not block or (block.t ~= "Plain" and block.t ~= "Para") then return nil end
  local kept = {}
  for _, inline in ipairs(block.content) do
    if inline.t ~= "RawInline" then kept[#kept + 1] = inline end
  end
  return kept
end

-- `<br>` on its own is a GitHub spacing hack under the summary. In LaTeX it would leave an empty
-- paragraph at the top of the box, so it goes.
local function is_spacer(block)
  return block.t == "Para" and #block.content == 1
      and block.content[1].t == "RawInline"
end

function Blocks(blocks)
  local out = {}
  local boxed = {}  -- stack: was this <details> turned into a box?
  local i = 1

  while i <= #blocks do
    local block = blocks[i]
    local tag = html(block)

    if tag and tag:match("^<details") then
      -- Look for <summary> / title / </summary> immediately after.
      local title = nil
      if html(blocks[i + 1]) == "<summary>" and html(blocks[i + 3]) == "</summary>" then
        title = summary_inlines(blocks[i + 2])
      end

      if title and pandoc.utils.stringify(title):find(TRYIT, 1, true) == 1 then
        out[#out + 1] = pandoc.RawBlock("latex", "\\begin{tryit}")
        out[#out + 1] = pandoc.Para{pandoc.Strong(title)}
        boxed[#boxed + 1] = true
        i = i + 4              -- consume <details>, <summary>, title, </summary>
        if i <= #blocks and is_spacer(blocks[i]) then i = i + 1 end
        goto continue
      end

      boxed[#boxed + 1] = false  -- not ours: drop the tag, as pandoc would have

    elseif tag == "</details>" then
      if boxed[#boxed] then out[#out + 1] = pandoc.RawBlock("latex", "\\end{tryit}") end
      boxed[#boxed] = nil

    else
      out[#out + 1] = block
    end

    i = i + 1
    ::continue::
  end

  return out
end
