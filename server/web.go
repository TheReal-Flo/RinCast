package main

import (
	"embed"
	"io/fs"
	"net/http"
	"strings"
)

//go:embed web
var webFS embed.FS

// webHandler serves the landing page at / and the single-page account site. Unknown paths
// fall back to index.html so client-side routes (/link, /account) work on reload.
func webHandler() http.Handler {
	sub, _ := fs.Sub(webFS, "web")
	files := http.FileServer(http.FS(sub))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := strings.TrimPrefix(r.URL.Path, "/")
		if path != "" {
			if f, err := sub.Open(path); err == nil {
				f.Close()
				files.ServeHTTP(w, r)
				return
			}
		}
		page := "index.html"
		if path == "" {
			page = "landing.html"
		}
		data, _ := fs.ReadFile(sub, page)
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-cache")
		_, _ = w.Write(data)
	})
}
