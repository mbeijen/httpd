-- Answer with the request body as r:requestbody() returns it, or with
-- "(none)" where it returns nothing.
function handle(r)
    r.content_type = "text/plain"
    local body = r:requestbody()
    if body == nil then
        r:puts("(none)")
    else
        r:puts("body=" .. body)
    end
    return apache2.OK
end
