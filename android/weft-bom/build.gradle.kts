plugins {
    id("java-platform")
    id("maven-publish")
}

javaPlatform {
    allowDependencies()
}

dependencies {
    constraints {
        api("dev.weft:weft-core:0.1.0")
        api("dev.weft:weft-compose:0.1.0")
    }
}

publishing {
    publications {
        create<MavenPublication>("bom") {
            from(components["javaPlatform"])
            groupId = "dev.weft"
            artifactId = "weft-bom"
            version = "0.1.0"
        }
    }
}
