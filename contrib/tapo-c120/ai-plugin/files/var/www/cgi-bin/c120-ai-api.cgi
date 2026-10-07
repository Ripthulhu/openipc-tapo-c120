#!/usr/bin/haserl
<%
# Majestic runs CGI through haserl, which puts a JSON request in POST_body.
printf '%s' "${POST_body:-}" | /usr/bin/c120-ai api
%>
