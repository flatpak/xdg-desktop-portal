# XDG Desktop Portal Website

This is the website for the [XDG Desktop Portal project](https://github.com/flatpak/xdg-desktop-portal).

## Setup

The process of setting up the site locally consists of:

- Install ruby [gem bundler](https://bundler.io/). On [Fedora](https://getfedora.org/)/in the [Toolbx](https://containertoolbx.org) you do:

```
toolbox enter
sudo dnf install rubygem-bundler
cd xdg-desktop-portal/doc/website
bundle install
```

- Test the site locally:

```
bundle exec jekyll s
```

- `git commit` your changes and create a merge request. After review, the merged changes get automatically deployed to the site.
